/*
 * Heart rate from one MAX30101 LED (chosen in the overlay).
 *
 * This file is the glue: it drains the sensor FIFO, feeds every sample to both
 * heart-rate algorithms, and once a second prints the status line and publishes one of
 * them for the BLE packet. The pieces:
 *   spectral_hr.c    frequency-domain tracker (published by default)
 *   classic_hr.c     autocorrelation estimate, for a still wrist
 *   bench.c          USB bench protocol for the testing pages
 *
 * One session starts at boot and runs continuously; finger placed/removed is handled
 * by the classic path's step-artifact detector. Switching the LED restarts the session.
 */

#include "hr_main.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "../motion/motion.h"
#include "bench.h"
#include "classic_hr.h"
#include "spectral_hr.h"

LOG_MODULE_DECLARE(blinky_main, LOG_LEVEL_INF);

#define MAX3010X_REG_FIFO_WR_PTR    0x04
#define MAX3010X_REG_FIFO_RD_PTR    0x06
#define MAX3010X_REG_LED1_PA        0x0C   /* LED1 red, LED2 IR, LED3 green */
#define MAX3010X_REG_MULTI_LED1     0x11   /* SLOT1 in bits [2:0], SLOT2 in [6:4] */
#define MAX3010X_FIFO_DEPTH         32

#define FS_NOMINAL   100.0f    /* 400 sps / 4x on-chip averaging */
#define MIN_ACDC     0.0003f   /* below this pulse amplitude the spectral reading isn't shown */
#define SAT_LEVEL    250000    /* ADC saturates at 262143; treat this as "too bright" */
#define AXES         3

#define PPG_NODE DT_NODELABEL(ppg)
static const struct device *ppg_sensor = DEVICE_DT_GET(PPG_NODE);
/* The Zephyr driver reads one FIFO sample per sensor_sample_fetch() and never looks at
 * the FIFO pointers, so we read them ourselves on the same bus. */
static const struct i2c_dt_spec ppg_i2c = I2C_DT_SPEC_GET(PPG_NODE);

/* The LED in slot 1 (1 red, 2 IR, 3 green) and its current, both from the overlay, so
 * the LED can be switched there without touching this file. */
#define PPG_SLOT1_LED DT_PROP_BY_IDX(PPG_NODE, led_slot, 0)
BUILD_ASSERT(PPG_SLOT1_LED >= 1 && PPG_SLOT1_LED <= 3, "ppg led-slot[0] must be 1, 2 or 3");
static const enum sensor_channel ppg_chan =
    PPG_SLOT1_LED == 1 ? SENSOR_CHAN_RED : PPG_SLOT1_LED == 2 ? SENSOR_CHAN_IR : SENSOR_CHAN_GREEN;
/* Optional second LED in slot 2 (0 = none). It doesn't feed the heart rate; it's
 * streamed as the raw line's second value so recordings carry two wavelengths for
 * offline work (e.g. combining green + IR against motion). */
#define PPG_SLOT2_LED DT_PROP_BY_IDX(PPG_NODE, led_slot, 1)
BUILD_ASSERT(PPG_SLOT2_LED >= 0 && PPG_SLOT2_LED <= 3, "ppg led-slot[1] must be 0-3");
BUILD_ASSERT(PPG_SLOT2_LED != PPG_SLOT1_LED, "ppg led-slot[1] must differ from slot 1");
static const enum sensor_channel ppg_chan2 =
    PPG_SLOT2_LED == 1 ? SENSOR_CHAN_RED : PPG_SLOT2_LED == 2 ? SENSOR_CHAN_IR : SENSOR_CHAN_GREEN;

/* The LED currently in slot 1. Starts as the overlay's choice; the bench "C<n>" command
 * swaps it at runtime. The driver keeps reading it as ppg_chan: it maps channels to FIFO
 * slots once at boot, so it returns slot 1's data whichever LED is lit there. */
static uint8_t ppgLed = PPG_SLOT1_LED;
/* led-pa is [red, IR, green], so the slot's LED number - 1 indexes it. Spelled out
 * because DT_PROP_BY_IDX(..., PPG_SLOT1_LED - 1) pastes the index as text. Changed at
 * runtime by the bench "L<n>" command. */
static uint8_t ledCurrent =
    PPG_SLOT1_LED == 1 ? DT_PROP_BY_IDX(PPG_NODE, led_pa, 0) :
    PPG_SLOT1_LED == 2 ? DT_PROP_BY_IDX(PPG_NODE, led_pa, 1) :
                         DT_PROP_BY_IDX(PPG_NODE, led_pa, 2);

/* Both algorithms always run; this picks which one is published (BLE packet, status
 * avgBpm=). The status line carries the other one as alt= for A/B on the testing page. */
static bool hrSpectral = true;

/* Session state */
static int64_t sessionStartMs;
static uint32_t sampleIdx;
static uint32_t lastTickSample;
static float fsHz = FS_NOMINAL; /* refined from the real sample count once a session is >10 s old */
static long lastRaw;

/* Published, read by hr_latest() */
static bool pubValid;
static int32_t pubBpm;

static const char *led_name(uint8_t led)
{
    return led == 1 ? "red" : led == 2 ? "IR" : "green";
}

static void startSession(void)
{
    fsHz = FS_NOMINAL;
    sampleIdx = 0;
    lastTickSample = 0;
    classic_hr_reset();
    spectral_hr_reset();
    pubValid = false;
    sessionStartMs = k_uptime_get();
    LOG_INF("SESSION START - warming up ~10s (2s settle + 8s window), keep finger still");
}

/* ---- bench commands ---- */

/* Put a different LED in slot 1 at the current drive level. A new wavelength changes
 * the DC level and pulse shape completely, so start a fresh session. */
static void set_led(uint8_t led)
{
    int err;

    if (led == ppgLed) {
        bench_log("LED set to %s (unchanged)", led_name(led));
        return;
    }
    /* Turn the old LED off unless slot 2 still uses it. */
    err = (ppgLed == PPG_SLOT2_LED) ? 0 :
          i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_LED1_PA + ppgLed - 1, 0);
    if (err == 0) {
        err = i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_LED1_PA + led - 1, ledCurrent);
    }
    if (err == 0) {
        /* Keep slot 2 as configured: the driver reads a fixed number of slots per
         * sample, so dropping it would misalign the FIFO. */
        err = i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_MULTI_LED1,
                                    led | (PPG_SLOT2_LED << 4));
    }
    if (err < 0) {
        bench_log("LED switch to %s failed (I2C err %d)", led_name(led), err);
        return;
    }
    ppgLed = led;
    bench_log("LED set to %s, current %u - restarting session", led_name(led), ledCurrent);
    startSession();
}

static void set_led_current(uint8_t pa)
{
    ledCurrent = pa;
    i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_LED1_PA + ppgLed - 1, ledCurrent);
    bench_log("LED current set to %u (the step detector will hold off briefly)", ledCurrent);
}

static void set_spectral(bool on)
{
    hrSpectral = on;
    bench_log("HR algorithm %s", on ? "spectral" : "classic");
}

static const struct bench_handlers bench_cmds = {
    .set_led = set_led,
    .set_led_current = set_led_current,
    .set_spectral = set_spectral,
};

/* ---- once a second ---- */

/* Refine the real sample rate, run both estimates, publish one, print status. */
static void onTick(void)
{
    int64_t elapsedMs = k_uptime_get() - sessionStartMs;

    if (elapsedMs >= 10000) {
        float est = sampleIdx * 1000.0f / (float)elapsedMs;

        if (est > 90.0f && est < 110.0f) { /* ignore nonsense (e.g. after a stall) */
            fsHz = est;
            classic_hr_set_fs(fsHz);
        }
    }

    struct classic_hr_status cl;
    uint32_t t0 = k_cycle_get_32();

    classic_hr_update(&cl);
    uint32_t clUs = k_cyc_to_us_floor32(k_cycle_get_32() - t0);

    if (cl.cleared) {
        LOG_INF("classic: no periodic pulse for %d s - reading cleared", CLASSIC_HR_MAX_AGE_SEC);
    }

    /* Spectral is only reported while there is a pulse signal at all, so it doesn't
     * invent a heart rate with the sensor off the skin. */
    float specBpm, sConf, sMot;
    bool specWarm = !spectral_hr_update(&specBpm, &sConf, &sMot);
    bool specValid = !specWarm && cl.acdc >= MIN_ACDC && lastRaw < SAT_LEVEL;

    if (hrSpectral) {
        pubValid = specValid;
        pubBpm = specValid ? (int32_t)(specBpm + 0.5f) : 0;
    } else {
        pubValid = cl.valid;
        pubBpm = cl.valid ? (int32_t)(cl.bpm + 0.5f) : 0;
    }

    char classicText[24], specText[24];

    if (cl.valid) {
        snprintf(classicText, sizeof(classicText), "%d", (int)(cl.bpm + 0.5f));
    } else if (cl.warm) {
        snprintf(classicText, sizeof(classicText), "warming(%d/%ds)",
                 (int)cl.filled_s, (int)cl.window_s);
    } else {
        snprintf(classicText, sizeof(classicText), "--");
    }
    if (specValid) {
        snprintf(specText, sizeof(specText), "%d", (int)(specBpm + 0.5f));
    } else if (specWarm) {
        snprintf(specText, sizeof(specText), "warming(%d/8s)", (int)MIN(elapsedMs / 1000, 8));
    } else {
        snprintf(specText, sizeof(specText), "--");
    }
    /* avgBpm is whichever algorithm is published; alt= is the other one. */
    const char *bpmText = hrSpectral ? specText : classicText;
    char hrText[48];

    snprintf(hrText, sizeof(hrText), " hr=%s alt=%s",
             hrSpectral ? "spec" : "classic", hrSpectral ? classicText : specText);

    /* mot: in-band wrist acceleration over the spectral window (m/s^2 rms), the number
     * the spectral tracker compares against its 0.2 threshold. Appended after est= so
     * the testing pages' status parsers still match. */
    char motText[16] = "";

    if (!specWarm && motion_available()) {
        snprintf(motText, sizeof(motText), " mot=%.2f", (double)sMot);
    }

    /* bench_log, not LOG_INF: the testing page needs this in bench mode too. */
    bench_log("status: ir=%ld avgBpm=%s led=%u acdc=%.2f%% r=%.2f fs=%.1f est=%.1fms%s%s",
              lastRaw, bpmText, ledCurrent, (double)(cl.acdc * 100.0f), (double)cl.r,
              (double)fsHz, (double)(clUs / 1000.0f), motText, hrText);
}

/* acc: accelerometer (m/s^2) aligned to this sample, or NULL if there is none. */
static void processSample(long raw, const float *acc)
{
    float filtered;

    sampleIdx++;
    lastRaw = raw;
    spectral_hr_add_sample((float)raw, acc);
    if (classic_hr_add_sample((float)raw, &filtered)) {
        LOG_INF("step artifact detected - resetting filters, holding off");
    }
    if (motion_available()) {
        bench_send_mc(filtered, acc);
    }
    if (sampleIdx - lastTickSample >= (uint32_t)fsHz) {
        lastTickSample = sampleIdx;
        onTick();
    }
}

/* ---- sensor I/O ---- */

int ppg_init(void)
{
    if (!device_is_ready(ppg_sensor)) {
        uint8_t part_id;
        int err;

        LOG_ERR("MAX30101 driver init failed at boot; probing the sensor directly");
        if (!device_is_ready(ppg_i2c.bus)) {
            LOG_ERR("I2C bus %s not ready", ppg_i2c.bus->name);
            return -ENODEV;
        }
        err = i2c_reg_read_byte_dt(&ppg_i2c, 0xFF, &part_id);
        if (err < 0) {
            LOG_ERR("No ACK from 0x%02x on %s (err %d): check power, SDA=D4, SCL=D5, pull-ups",
                    ppg_i2c.addr, ppg_i2c.bus->name, err);
        } else {
            LOG_ERR("Sensor answers at 0x%02x, part ID 0x%02x (driver expects 0x15)",
                    ppg_i2c.addr, part_id);
        }
        return -ENODEV;
    }

    LOG_INF("MAX30101 init succeeded (%s only, LED 0x%02X, spectral + autocorrelation)",
            led_name(ppgLed), ledCurrent);
    bench_init(&bench_cmds);
    startSession();
    return 0;
}

/* Read every sample waiting in the FIFO and run each one through the pipeline. */
void ppg_poll(void)
{
    static float accPrev[AXES];
    static bool accPrevOk;
    float accNow[AXES];
    uint8_t wr, rd;
    int pending;

    /* One IMU reading per poll, taken just before the FIFO read so it lines up with the
     * newest PPG sample; the samples in between are interpolated from the previous
     * poll's reading (polls are 50 ms apart, motion is < 4 Hz). */
    bool accOk = motion_available() && motion_read_accel(accNow) == 0;

    if (i2c_reg_read_byte_dt(&ppg_i2c, MAX3010X_REG_FIFO_WR_PTR, &wr) < 0 ||
        i2c_reg_read_byte_dt(&ppg_i2c, MAX3010X_REG_FIFO_RD_PTR, &rd) < 0) {
        LOG_ERR("Failed to read MAX30101 FIFO pointers");
        return;
    }

    /* WR == RD is treated as empty. OVF_COUNTER reads nonzero on this board even while
     * every sample is being drained, so it can't tell "full" from "empty". */
    pending = ((wr & 0x1F) - (rd & 0x1F)) & (MAX3010X_FIFO_DEPTH - 1);
    if (pending >= MAX3010X_FIFO_DEPTH - 4) {
        LOG_WRN("MAX30101 FIFO nearly full (%d samples): poll is running late", pending);
    }

    for (int i = 0; i < pending; i++) {
        struct sensor_value sample;

        /* Each fetch pops one sample (one slot) from FIFO_DATA */
        if (sensor_sample_fetch(ppg_sensor) < 0) {
            LOG_ERR("Failed to fetch MAX30101 sample");
            break;
        }
        sensor_channel_get(ppg_sensor, ppg_chan, &sample);

        /* Slot 2 (if any) only goes out on the raw stream, as its second value. */
        struct sensor_value sample2 = sample;

        if (PPG_SLOT2_LED != 0) {
            sensor_channel_get(ppg_sensor, ppg_chan2, &sample2);
        }

        float acc[AXES];
        const float *accp = NULL;

        if (accOk) {
            float t = (float)(i + 1) / (float)pending;

            for (int a = 0; a < AXES; a++) {
                acc[a] = accPrevOk ? accPrev[a] + (accNow[a] - accPrev[a]) * t : accNow[a];
            }
            accp = acc;
        }

        bench_send_raw(sample.val1, sample2.val1);
        processSample(sample.val1, accp);
    }

    if (accOk) {
        memcpy(accPrev, accNow, sizeof(accPrev));
    }
    accPrevOk = accOk;

    bench_poll();
}

bool hr_latest(int32_t *bpm)
{
    if (!pubValid) {
        return false;
    }
    *bpm = pubBpm;
    return true;
}
