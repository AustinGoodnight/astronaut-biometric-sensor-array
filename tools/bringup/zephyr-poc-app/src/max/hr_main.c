#ifndef HR_MAIN_C
#define HR_MAIN_C

/*
 * Heart rate from one MAX30101 LED (chosen in the overlay; currently IR).
 *
 * Direct port of the ESP32-C6 bring-up firmware
 * tools/bringup/hr-max30101-ble/src/hr_dual_channel_patched_6.cpp: same sensor
 * setup, filter coefficients, thresholds, decision logic and log lines. Read
 * that file's header for why each piece exists. Function and variable names
 * are kept the same so the two can be compared side by side.
 *
 * Differences from the ESP32 version:
 *  - Sensor setup comes from devicetree (xiao_ble_nrf52840.overlay) instead of
 *    SparkFun setup(). Same registers, except the LED: the ESP32 uses green,
 *    which was too weak here, so the overlay currently selects IR.
 *  - No start/stop button. The ESP32's button is on D0, which is the
 *    skin-resistance ADC input on this board, so one session starts at boot
 *    and runs continuously. Finger placed/removed is handled by the same
 *    step-artifact detector the ESP32 uses mid-session.
 *  - No raw-sample BLE characteristic and no 'L' LED-current serial command.
 *  - The result is handed to main.c (latest_heart_rate / heart_rate_valid)
 *    for the telemetry packet instead of BLE text lines.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_ctrl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "../motion/motion.h"

/* The FIFO fills at 100 samples/s and holds 32 (320 ms). Each poll drains it,
 * so this only needs to be comfortably shorter than 320 ms. */
#define PPG_POLL_PERIOD_MS          50

#define MAX3010X_REG_FIFO_WR_PTR    0x04
#define MAX3010X_REG_FIFO_RD_PTR    0x06
#define MAX3010X_FIFO_DEPTH         32

#define PPG_NODE DT_NODELABEL(ppg)
static const struct device *ppg_sensor = DEVICE_DT_GET(PPG_NODE);
/* The Zephyr driver reads one FIFO sample per sensor_sample_fetch() and never
 * looks at the FIFO pointers, so we read them ourselves on the same bus. */
static const struct i2c_dt_spec ppg_i2c = I2C_DT_SPEC_GET(PPG_NODE);

/* Read by main.c for the telemetry packet. */
static int32_t latest_heart_rate = 0;
static int8_t heart_rate_valid = 0;

/* The LED in slot 1 (1 red, 2 IR, 3 green) and its current, both from the
 * overlay, so the LED can be switched there without touching this file. */
#define PPG_SLOT1_LED DT_PROP_BY_IDX(PPG_NODE, led_slot, 0)
BUILD_ASSERT(PPG_SLOT1_LED >= 1 && PPG_SLOT1_LED <= 3, "ppg led-slot[0] must be 1, 2 or 3");
static const enum sensor_channel ppg_chan =
    PPG_SLOT1_LED == 1 ? SENSOR_CHAN_RED : PPG_SLOT1_LED == 2 ? SENSOR_CHAN_IR : SENSOR_CHAN_GREEN;
/* The LED currently in slot 1. Starts as the overlay's choice; the bench "C<n>"
 * command swaps it at runtime. The driver still reads it as ppg_chan: only slot 1
 * is active, so it returns slot 1's data whichever LED is lit. */
static uint8_t ppgLed = PPG_SLOT1_LED;
static const char *led_name(uint8_t led)
{
    return led == 1 ? "red" : led == 2 ? "IR" : "green";
}
/* led-pa is [red, IR, green], so the slot's LED number - 1 indexes it. Spelled out
 * because DT_PROP_BY_IDX(..., PPG_SLOT1_LED - 1) pastes the index as text.
 * Changed at runtime by the bench "L<n>" command. */
static uint8_t ledCurrent =
    PPG_SLOT1_LED == 1 ? DT_PROP_BY_IDX(PPG_NODE, led_pa, 0) :
    PPG_SLOT1_LED == 2 ? DT_PROP_BY_IDX(PPG_NODE, led_pa, 1) :
                         DT_PROP_BY_IDX(PPG_NODE, led_pa, 2);
#define MAX3010X_REG_LED1_PA        0x0C   /* LED1 red, LED2 IR, LED3 green */

/* ============================================================================
 * USB bench mode: the ESP32 firmware's USB serial protocol, so the HR testing
 * page (hr-max30101-ble/ppg_dashboard.html, "Connect USB") works unchanged.
 * The page sends 'R' to start the raw stream; from then on only the ESP32's
 * lines go out over USB (status:, beat:, raw,...) and the normal Zephyr log
 * is muted. 'r', or the page closing the port, turns it off again.
 * 'L<0-255>' sets the LED current. 'C<1-3>' switches the LED (1 red, 2 IR,
 * 3 green) at the same current and restarts the session. 'M' / 'm' turn motion
 * cancelling on / off (Sense only). The ESP32 ignores 'C' and 'M'.
 * Bluetooth is not affected.
 * ========================================================================== */

static const struct device *const bench_uart = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
static bool serialRawOn; /* set by 'R' / cleared by 'r' received over USB serial */

static void bench_write(const char *s, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uart_poll_out(bench_uart, s[i]); /* CDC ACM drops (never blocks) if the host stalls */
    }
}

static void bench_set(bool on)
{
    const struct log_backend *backend = log_backend_get_by_name("log_backend_uart");

    if (on == serialRawOn) {
        return;
    }
    if (!on) {
        serialRawOn = false;
    }
    if (backend != NULL) {
        if (on) {
            log_backend_disable(backend);
        } else {
            log_backend_enable(backend, backend->cb->ctx, CONFIG_LOG_MAX_LEVEL);
        }
    }
    if (on) {
        serialRawOn = true;
    } else {
        LOG_INF("USB bench mode off, normal logging resumed");
    }
}

/* The ESP32's logLine(): a plain line on USB in bench mode, a normal log line otherwise. */
static void logLine(const char *fmt, ...)
{
    char line[192];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if (n >= (int)sizeof(line)) {
        n = sizeof(line) - 1;
    }

    if (serialRawOn) {
        bench_write(line, n);
        bench_write("\r\n", 2);
    } else {
        LOG_INF("%s", line);
    }
}

/* Same batching as the ESP32's sendRawSample(): one line per 24 samples,
 * "raw,<seq>,<ir>:<red>,..." (only in bench mode). */
#define RAW_BATCH_SIZE 24
static uint32_t rawBatchIr[RAW_BATCH_SIZE];
static uint32_t rawBatchRed[RAW_BATCH_SIZE];
static uint8_t rawBatchCount;
static uint16_t rawSeq;

static void sendRawSample(uint32_t irRaw, uint32_t redRaw)
{
    rawBatchIr[rawBatchCount] = irRaw;
    rawBatchRed[rawBatchCount] = redRaw;
    rawBatchCount++;

    if (rawBatchCount < RAW_BATCH_SIZE) {
        return;
    }
    rawBatchCount = 0;
    uint16_t seq = rawSeq++;

    if (!serialRawOn) {
        return;
    }
    char line[16 + RAW_BATCH_SIZE * 16];
    int n = snprintf(line, sizeof(line), "raw,%u", (unsigned)seq);
    for (int i = 0; i < RAW_BATCH_SIZE && n < (int)sizeof(line) - 16; i++) {
        n += snprintf(line + n, sizeof(line) - n, ",%lu:%lu",
                      (unsigned long)rawBatchIr[i], (unsigned long)rawBatchRed[i]);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    bench_write(line, n);
}

/* Commands from the HR testing page, parsed like the ESP32's loop(). */
#define MAX3010X_REG_MULTI_LED1     0x11   /* SLOT1 in bits [2:0], SLOT2 in [6:4] */

static void startSession(void);
static void mc_set(bool on);

/* Put a different LED in slot 1 at the current drive level. A new wavelength
 * changes the DC level and pulse shape completely, so start a fresh session. */
static void ppg_set_led(uint8_t led)
{
    int err;

    if (led == ppgLed) {
        logLine("LED set to %s (unchanged)", led_name(led));
        return;
    }
    err = i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_LED1_PA + ppgLed - 1, 0);
    if (err == 0) {
        err = i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_LED1_PA + led - 1, ledCurrent);
    }
    if (err == 0) {
        err = i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_MULTI_LED1, led); /* slot 2 off */
    }
    if (err < 0) {
        logLine("LED switch to %s failed (I2C err %d)", led_name(led), err);
        return;
    }
    ppgLed = led;
    logLine("LED set to %s, current %u - restarting session", led_name(led), ledCurrent);
    startSession();
}

static void bench_poll_rx(void)
{
    static bool ledParsing, colorParsing;
    static int ledVal;
    unsigned char c;
    uint32_t dtr = 0;

    while (uart_poll_in(bench_uart, &c) == 0) {
        if (colorParsing) { /* the one character after 'C' */
            colorParsing = false;
            if (c >= '1' && c <= '3') {
                ppg_set_led(c - '0');
            }
        } else if (c == 'C') {
            colorParsing = true;
        } else if (c == 'M' || c == 'm') {
            mc_set(c == 'M');
        } else if (c == 'L') {
            ledParsing = true;
            ledVal = 0;
        } else if (ledParsing && c >= '0' && c <= '9') {
            if (ledVal < 1000) {
                ledVal = ledVal * 10 + (c - '0');
            }
        } else if (ledParsing) { /* any non-digit terminates the number */
            ledParsing = false;
            ledCurrent = (uint8_t)MIN(ledVal, 255);
            i2c_reg_write_byte_dt(&ppg_i2c, MAX3010X_REG_LED1_PA + ppgLed - 1, ledCurrent);
            logLine("LED current set to %u (the step detector will hold off briefly)", ledCurrent);
        } else if (c == 'R') {
            bench_set(true);
        } else if (c == 'r') {
            bench_set(false);
        }
    }

    /* Page closed without sending 'r': the host dropped DTR. */
    if (serialRawOn && uart_line_ctrl_get(bench_uart, UART_LINE_CTRL_DTR, &dtr) == 0 && !dtr) {
        bench_set(false);
    }
}

/* ============================================================================
 * Signal pipeline
 * ========================================================================== */

#define FS_NOMINAL 100.0f          /* 400 sps / 4x on-chip averaging */
static float fsHz = FS_NOMINAL;    /* refined from real sample count once a session is >10 s old */

/* --- 2nd-order Butterworth bandpass, 0.7-3.5 Hz @ 100 Hz (two cascaded biquads) ---
 * Coefficients from scipy.signal.butter(2, [0.7, 3.5], 'bandpass', fs=100, output='sos').
 * They assume fs=100; a few % of sample-rate error just shifts the band edges slightly. */
struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1, z2;
};

static float biquad_process(struct Biquad *q, float x) /* transposed direct form II */
{
    float y = q->b0 * x + q->z1;

    q->z1 = q->b1 * x - q->a1 * y + q->z2;
    q->z2 = q->b2 * x - q->a2 * y;
    return y;
}

struct PpgFilter {
    struct Biquad s0, s1;
};

#define PPG_FILTER_INIT {                                                            \
    .s0 = { 0.006867866f, 0.013735731f, 0.006867866f, -1.786023501f, 0.820363944f }, \
    .s1 = { 1.0f, -2.0f, 1.0f, -1.948065846f, 0.950479924f },                        \
}

static float filter_process(struct PpgFilter *f, float x)
{
    return biquad_process(&f->s1, biquad_process(&f->s0, x));
}

static void filter_reset(struct PpgFilter *f)
{
    f->s0.z1 = f->s0.z2 = 0;
    f->s1.z1 = f->s1.z2 = 0;
}

static struct PpgFilter irFilter = PPG_FILTER_INIT;
static struct PpgFilter redFilter = PPG_FILTER_INIT;

/* --- Motion cancelling: NLMS adaptive filter, accelerometer as the noise reference ---
 * Runs only when the IMU came up at boot (motion_available(): Sense build with a
 * working IMU), which is what the boot banner's "motion cancel ON" reports.
 * The accelerometer goes through the same 0.7-3.5 Hz bandpass as the PPG (which
 * also strips gravity); the filter learns how that motion leaks into the PPG and
 * subtracts it. Bench commands 'M' / 'm' switch it on/off for A/B testing. */
#define MC_TAPS   8                     /* per axis: 80 ms of history at 100 sps */
#define MC_AXES   3
#define MC_N      (MC_TAPS * MC_AXES)
#define MC_MU     0.05f                 /* step size: faster adaptation vs. more pulse distortion */
#define MC_REST   0.05f                 /* m/s^2 rms per axis in-band with the wrist still */
#define MC_EPS    (MC_N * MC_REST * MC_REST) /* below this motion level adaptation fades out, so
                                          * the filter doesn't learn sensor noise or the
                                          * wrist's own pulse-driven movement */
#define MC_LEAK   0.9995f               /* weights decay over ~20 s when there's nothing to cancel */

struct Nlms {
    float w[MC_N];
};
static struct Nlms irNlms, redNlms;
static struct PpgFilter accFilter[MC_AXES] = { PPG_FILTER_INIT, PPG_FILTER_INIT, PPG_FILTER_INIT };
static float accDc[MC_AXES];
static float mcRef[MC_N];       /* tap history, [axis * MC_TAPS + k], k = 0 newest */
static float mcRefPow;          /* sum of squares of mcRef */
static bool mcPrimed;
static bool mcEnabled;          /* set in ppg_init() from motion_available() */
static float mcInPow, mcOutPow; /* smoothed PPG power before/after, for the status line */
static float mcMotPow;          /* smoothed in-band accel power, for the status line */

static void mc_reset(void)
{
    memset(&irNlms, 0, sizeof(irNlms));
    memset(&redNlms, 0, sizeof(redNlms));
    for (int a = 0; a < MC_AXES; a++) {
        filter_reset(&accFilter[a]);
    }
    memset(mcRef, 0, sizeof(mcRef));
    mcRefPow = mcInPow = mcOutPow = mcMotPow = 0;
    mcPrimed = false;
}

/* Push one accelerometer sample (m/s^2) into the reference taps. */
static void mc_push_ref(const float acc[MC_AXES])
{
    float newest = 0;

    if (!mcPrimed) { /* start the DC tracker on gravity so the bandpass doesn't ring */
        for (int a = 0; a < MC_AXES; a++) {
            accDc[a] = acc[a];
        }
        mcPrimed = true;
    }
    mcRefPow = 0;
    for (int a = 0; a < MC_AXES; a++) {
        float in = acc[a] - accDc[a];
        float *h = &mcRef[a * MC_TAPS];

        accDc[a] += 0.01f * in; /* same ~1 s tracker as the PPG's DC_ALPHA */
        memmove(&h[1], &h[0], (MC_TAPS - 1) * sizeof(float));
        h[0] = filter_process(&accFilter[a], in);
        newest += h[0] * h[0];
        for (int k = 0; k < MC_TAPS; k++) {
            mcRefPow += h[k] * h[k];
        }
    }
    mcMotPow += 0.01f * (newest - mcMotPow);
}

/* One NLMS step: returns x with the motion estimate removed. Adapts only if asked
 * (not during a holdoff, when the PPG filters have just been reset). */
static float mc_cancel(struct Nlms *f, float x, bool adapt)
{
    float y = 0;

    for (int i = 0; i < MC_N; i++) {
        y += f->w[i] * mcRef[i];
    }
    float e = x - y;

    if (adapt) {
        float g = MC_MU * e / (MC_EPS + mcRefPow);

        for (int i = 0; i < MC_N; i++) {
            f->w[i] = MC_LEAK * f->w[i] + g * mcRef[i];
        }
    }
    return e;
}

static void mc_set(bool on)
{
    if (on && !motion_available()) {
        logLine("motion cancel unavailable (no IMU on this build/board)");
        return;
    }
    if (on != mcEnabled) {
        mc_reset();
        mcEnabled = on;
    }
    logLine("motion cancel %s", on ? "ON" : "OFF");
}

/* Bench stream of the filtered PPG before and after motion cancelling, so the
 * HR testing page can overlay them: "mc,<seq>,<before>:<after>,..." in hundredths
 * of an ADC count, one line per raw batch and sent right after it with the same
 * <seq>. Sent whenever the IMU is present; with cancelling off, after == before. */
static int16_t mcBatchPre[RAW_BATCH_SIZE], mcBatchPost[RAW_BATCH_SIZE];
static uint8_t mcBatchCount;

static int16_t mc_to_i16(float v)
{
    return (int16_t)CLAMP(v * 100.0f, -32767.0f, 32767.0f);
}

static void mc_record(float before, float after)
{
    mcBatchPre[mcBatchCount] = mc_to_i16(before);
    mcBatchPost[mcBatchCount] = mc_to_i16(after);
    if (++mcBatchCount < RAW_BATCH_SIZE) {
        return;
    }
    mcBatchCount = 0;
    if (!serialRawOn) {
        return;
    }

    /* processSample() runs right after sendRawSample() for every sample, so this
     * batch covers the same samples as the raw batch just sent. */
    char line[16 + RAW_BATCH_SIZE * 14];
    int n = snprintf(line, sizeof(line), "mc,%u", (unsigned)(uint16_t)(rawSeq - 1));

    for (int i = 0; i < RAW_BATCH_SIZE && n < (int)sizeof(line) - 14; i++) {
        n += snprintf(line + n, sizeof(line) - n, ",%d:%d", mcBatchPre[i], mcBatchPost[i]);
    }
    line[n++] = '\r';
    line[n++] = '\n';
    bench_write(line, n);
}

/* Slow DC estimates, subtracted BEFORE the bandpass so the filters never see the ~85k DC
 * as a giant startup step. Also used as the denominator of the AC/DC quality ratio. */
#define DC_ALPHA 0.01f   /* ~1 s time constant at 100 sps */
static float irDc, redDc;
static float envAbs;     /* EMA of |filtered IR| ~ pulse amplitude */
#define ENV_ALPHA 0.02f

static float prevIrRaw;
static float irPrevFiltered;
static float redPrevFiltered;
static float redSlopeAvg;   /* smoothed Red slope, single-pole (see corroboration check) */

/* --- Tunables --- */
#define MIN_ACDC           0.0003f  /* edge-detector gate only (mean|AC|/DC) */
#define STEP_FRAC          0.03f    /* sample-to-sample jump > 3% of DC = artifact (real beats ~0.5%) */
#define HOLDOFF_STEP_SEC   1.5f     /* detection pause after an artifact */
#define SETTLE_SEC         2.0f     /* detection pause at session start */
#define SLOPE_GATE         0.02f    /* falling edge must drop >= 2% of envelope in one sample */
#define REFRACTORY_MIN_SEC 0.30f    /* caps at 200 bpm */
#define REFRACTORY_FRAC    0.65f    /* ...but never less than 65% of the current beat period */
#define SAT_LEVEL          250000   /* ADC saturates at 262143; treat this as "too bright" */

/* --- Session / timing state (everything measured in samples since session start) --- */
static int64_t sessionStartMs;
static uint32_t sampleIdx;
static uint32_t holdoffUntil;
static uint32_t lastTickSample;
static bool needInit = true;

/* --- Sliding-window autocorrelation heart rate --- */
#define WIN_DECIM       4
#define WIN_LEN         200     /* 8 s @ 25 Hz */
#define MAX_LAG         40      /* covers 42 bpm (lag ~36 @ 25 Hz) with margin */
#define HR_JUMP_BPM     15.0f   /* a window this far from the published value must persist... */
#define HR_JUMP_CONFIRM 3       /* ...for this many consecutive windows (within 10 bpm of each other) */
#define HR_FAIL_TICKS   5       /* this many failed windows in a row clears the reading */
#define AC_MIN_CORR     0.50f   /* windows less periodic than this are not published */
#define HR_HIST         5       /* median over the last 5 estimates */
#define HR_STALE_SEC    15.0f   /* drop the reading if no valid estimate for this long */
static float winBuf[WIN_LEN];
static int winHead, winCount;
static float decAcc;            /* running sum for 4x decimation */
static uint8_t decCount;
static float lastR;             /* autocorrelation peak of the last window (0..1, want > ~0.5) */
static uint32_t acUs;           /* how long the last estimate took */
static uint8_t failTicks;
static float pendingBpm;        /* candidate for a big jump, awaiting confirmation */
static uint8_t pendingCount;
static float hrHist[HR_HIST];
static uint8_t hrHistSpot, hrHistCount;
static float hrEstimate;
static bool hrValid;
static uint32_t lastHrSample;

/* --- Edge-based beat markers (validated against hrEstimate) --- */
static bool haveLastBeat;
static uint32_t lastBeatSample;
#define RATE_SIZE 9
static uint8_t rates[RATE_SIZE];
static uint8_t rateSpot;
static uint8_t beatsCollected;
static int beatMedian;

/* ---------------------------------------------------------------------------
 * Autocorrelation heart-rate estimate over the current window.
 * A periodic pulse correlates strongly with itself shifted by one beat period. Lags are
 * searched only over the human range (42-220 bpm), and the *first* strong peak is taken
 * rather than the global max, since multiples of the true period (half the bpm) correlate
 * almost as well. The peak is refined with parabolic interpolation (1 sample ~ 1.5 bpm).
 * ------------------------------------------------------------------------- */
static bool estimateHr(float *bpmOut, float *rOut)
{
    static float x[WIN_LEN];
    static float ac[MAX_LAG + 2];
    int n = winCount;
    int start = (winHead - n + WIN_LEN) % WIN_LEN; /* oldest sample in the ring */

    float mean = 0;
    for (int i = 0; i < n; i++) {
        x[i] = winBuf[(start + i) % WIN_LEN];
        mean += x[i];
    }
    mean /= n;
    float r0 = 0;
    for (int i = 0; i < n; i++) {
        x[i] -= mean;
        r0 += x[i] * x[i];
    }
    r0 /= n;
    if (r0 <= 0) {
        return false;
    }

    float fsWin = fsHz / WIN_DECIM;
    int lagMin = (int)(fsWin * 60.0f / 220.0f);
    int lagMax = (int)(fsWin * 60.0f / 42.0f + 0.5f);
    if (lagMin < 2) {
        lagMin = 2;
    }
    if (lagMax > MAX_LAG - 1) {
        lagMax = MAX_LAG - 1;
    }

    for (int lag = lagMin - 1; lag <= lagMax + 1; lag++) {
        float sum = 0;
        for (int i = 0; i + lag < n; i++) {
            sum += x[i] * x[i + lag];
        }
        ac[lag] = (sum / (n - lag)) / r0; /* normalized: 1.0 = perfectly periodic at this lag */
    }

    float rmax = 0;
    for (int lag = lagMin; lag <= lagMax; lag++) {
        rmax = fmaxf(rmax, ac[lag]);
    }
    *rOut = rmax;
    if (rmax < AC_MIN_CORR) {
        return false;
    }

    int best = -1;
    for (int lag = lagMin; lag <= lagMax; lag++) {
        if (ac[lag] >= ac[lag - 1] && ac[lag] >= ac[lag + 1] && ac[lag] >= 0.85f * rmax) {
            best = lag;
            break;
        }
    }
    if (best < 0) {
        return false;
    }

    float a = ac[best - 1], b = ac[best], c = ac[best + 1];
    float denom = a - 2.0f * b + c;
    float delta = (denom != 0.0f) ? 0.5f * (a - c) / denom : 0.0f;
    if (delta > 1.0f) {
        delta = 1.0f;
    }
    if (delta < -1.0f) {
        delta = -1.0f;
    }
    *bpmOut = 60.0f * fsWin / ((float)best + delta);
    return true;
}

/* Median of the last few window estimates, so one odd window can't move the reading. */
static void publishHr(float bpm)
{
    /* Jump guard: a real HR change of >15 bpm doesn't happen in one second. A window that
     * disagrees that much with the published value is held back until it repeats. */
    if (hrValid && fabsf(bpm - hrEstimate) > HR_JUMP_BPM) {
        if (pendingCount > 0 && fabsf(bpm - pendingBpm) <= 10.0f) {
            pendingCount++;
        } else {
            pendingCount = 1;
        }
        pendingBpm = bpm;
        if (pendingCount < HR_JUMP_CONFIRM) {
            return; /* hold the current reading */
        }
        hrHistCount = 0; /* persisted: adopt it, restart the median */
        hrHistSpot = 0;
    }
    pendingCount = 0;

    hrHist[hrHistSpot++] = bpm;
    hrHistSpot %= HR_HIST;
    if (hrHistCount < HR_HIST) {
        hrHistCount++;
    }

    float tmp[HR_HIST];
    memcpy(tmp, hrHist, hrHistCount * sizeof(float));
    for (int i = 1; i < hrHistCount; i++) {
        float key = tmp[i];
        int j = i - 1;
        while (j >= 0 && tmp[j] > key) {
            tmp[j + 1] = tmp[j];
            j--;
        }
        tmp[j + 1] = key;
    }
    hrEstimate = tmp[hrHistCount / 2];
    hrValid = true;
    lastHrSample = sampleIdx;
}

static void clearHr(void)
{
    pendingCount = 0;
    hrValid = false;
    hrHistCount = 0;
    hrHistSpot = 0;
}

/* ---------------------------------------------------------------------------
 * Beat markers. Returns true if this edge should become the timing reference for the
 * NEXT interval. Edges that are clearly false (too fast) do not, so a notch or noise
 * spike can't corrupt the following interval. Edges that follow a missed beat DO
 * (resync), but aren't scored.
 * ------------------------------------------------------------------------- */
static bool handleBeat(float instBpm)
{
    if (!hrValid) {
        LOG_INF("beat: %.1f bpm -> unscored (waiting for window estimate)", (double)instBpm);
        return true;
    }

    float ratio = instBpm / hrEstimate;
    if (ratio > 1.25f) {
        LOG_INF("beat: %.1f bpm -> IGNORED (too fast vs %d - likely false edge)",
                (double)instBpm, (int)(hrEstimate + 0.5f));
        return false;
    }
    if (ratio < 0.75f) {
        LOG_INF("beat: %.1f bpm -> RESYNC (too slow vs %d - likely missed beat)",
                (double)instBpm, (int)(hrEstimate + 0.5f));
        return true;
    }

    /* (byte) cast on the ESP32; clamped here since a float > 255 -> uint8_t is undefined in C. */
    rates[rateSpot++] = (uint8_t)fminf(instBpm, 255.0f);
    rateSpot %= RATE_SIZE;
    if (beatsCollected < RATE_SIZE) {
        beatsCollected++;
    }

    uint8_t sorted[RATE_SIZE];
    memcpy(sorted, rates, beatsCollected);
    for (int i = 1; i < beatsCollected; i++) {
        uint8_t key = sorted[i];
        int j = i - 1;
        while (j >= 0 && sorted[j] > key) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
    beatMedian = sorted[beatsCollected / 2];

    LOG_INF("beat: %.1f bpm -> KEPT (beat median %d, window est %d)",
            (double)instBpm, beatMedian, (int)(hrEstimate + 0.5f));
    return true;
}

static void onEdge(void)
{
    if (!haveLastBeat) { /* first edge after start/artifact: timing reference only */
        lastBeatSample = sampleIdx;
        haveLastBeat = true;
        return;
    }

    uint32_t interval = sampleIdx - lastBeatSample;
    float minInterval = REFRACTORY_MIN_SEC * fsHz;
    if (hrValid) {
        minInterval = fmaxf(minInterval, REFRACTORY_FRAC * 60.0f * fsHz / hrEstimate);
    }
    if ((float)interval < minInterval) {
        return; /* inside refractory: silently ignore */
    }

    /* A real pulse shows up as an absorption change on both LEDs at essentially the same
     * instant; single-channel noise usually won't. The beat is a dip, so Red must also be
     * falling. (Green-only: green is fed in as both channels, so this always agrees.) */
    if (redSlopeAvg >= 0) {
        LOG_INF("candidate beat REJECTED (no Red-channel corroboration)");
        return;
    }

    float instBpm = 60.0f * fsHz / (float)interval;
    if (handleBeat(instBpm)) {
        lastBeatSample = sampleIdx;
    }
}

/* Drop filter/window state after an artifact (finger moved/removed, etc.) so its
 * transient can't masquerade as beats or poison the autocorrelation window. */
static void startHoldoff(float irRaw, float redRaw, float seconds)
{
    filter_reset(&irFilter);
    filter_reset(&redFilter);
    irDc = irRaw;
    redDc = redRaw;
    irPrevFiltered = redPrevFiltered = redSlopeAvg = 0;
    winHead = winCount = 0;
    decAcc = 0;
    decCount = 0;
    haveLastBeat = false;
    holdoffUntil = sampleIdx + (uint32_t)(seconds * fsHz);
}

/* Once a second: refine the real sample rate, run the window estimate, print status. */
static void onTick(long irRaw)
{
    int64_t elapsedMs = k_uptime_get() - sessionStartMs;
    if (elapsedMs >= 10000) {
        float est = sampleIdx * 1000.0f / (float)elapsedMs;
        if (est > 90.0f && est < 110.0f) {
            fsHz = est; /* ignore nonsense (e.g. after a stall) */
        }
    }

    if (hrValid && (float)(sampleIdx - lastHrSample) > HR_STALE_SEC * fsHz) {
        clearHr();
    }

    if (winCount >= WIN_LEN) {
        float bpm;
        uint32_t t0 = k_cycle_get_32();
        bool ok = estimateHr(&bpm, &lastR);
        acUs = k_cyc_to_us_floor32(k_cycle_get_32() - t0);
        if (ok) {
            publishHr(bpm);
            failTicks = 0;
        } else {
            if (failTicks < 255) {
                failTicks++;
            }
            if (failTicks >= HR_FAIL_TICKS && hrValid) { /* finger off / no periodic pulse */
                clearHr();
                LOG_INF("no periodic pulse for %d windows - reading cleared", HR_FAIL_TICKS);
            }
        }
    }

    float acdc = (irDc > 0) ? envAbs / irDc : 0;
    char bpmText[24];
    if (hrValid) {
        snprintf(bpmText, sizeof(bpmText), "%d", (int)(hrEstimate + 0.5f));
    } else if (winCount < WIN_LEN) {
        snprintf(bpmText, sizeof(bpmText), "warming(%d/%ds)",
                 (int)(winCount * WIN_DECIM / fsHz), (int)(WIN_LEN * WIN_DECIM / fsHz));
    } else {
        snprintf(bpmText, sizeof(bpmText), "--");
    }

    /* mc: motion cancel on/off; mot: in-band wrist acceleration (m/s^2 rms);
     * cut: share of PPG power the filter removed (pulse + motion). Appended after
     * est= so the HR testing page's status parser still matches. */
    char mcText[48] = " mc=off";
    if (mcEnabled) {
        float cut = (mcInPow > 0) ? 1.0f - mcOutPow / mcInPow : 0;
        snprintf(mcText, sizeof(mcText), " mc=on mot=%.2f cut=%d%%",
                 (double)sqrtf(mcMotPow), (int)(CLAMP(cut, -9.99f, 1.0f) * 100.0f));
    }

    /* logLine, not LOG_INF: the testing page needs this in bench mode too (the
     * ESP32 sends it there), to plot the device's own (motion-cancelled) estimate. */
    logLine("status: ir=%ld avgBpm=%s led=%u acdc=%.2f%% r=%.2f fs=%.1f est=%.1fms%s",
            irRaw, bpmText, ledCurrent, (double)(acdc * 100.0f), (double)lastR,
            (double)fsHz, (double)(acUs / 1000.0f), mcText);
}

/* acc: accelerometer (m/s^2) aligned to this sample, or NULL if there is none. */
static void processSample(long irRaw, long redRaw, const float *acc)
{
    float ir = (float)irRaw;
    float red = (float)redRaw;
    sampleIdx++;

    if (needInit) { /* first sample of the session */
        needInit = false;
        irDc = ir;
        redDc = red;
        prevIrRaw = ir;
        holdoffUntil = (uint32_t)(SETTLE_SEC * fsHz);
    }

    /* Artifact detector: a real beat moves ~0.5% of DC per sample; anything > 3% is a
     * step (finger shift/removal, LED or register change), not physiology. */
    if (fabsf(ir - prevIrRaw) > STEP_FRAC * irDc) {
        if (sampleIdx >= holdoffUntil) {
            LOG_INF("step artifact detected - resetting filters, holding off");
        }
        /* A huge step (>10% of DC) is a finger being removed/placed, not a nudge: the old
         * reading is no longer trustworthy, so blank it and re-warm. */
        if (fabsf(ir - prevIrRaw) > 0.10f * irDc && hrValid) {
            clearHr();
        }
        startHoldoff(ir, red, HOLDOFF_STEP_SEC);
    }
    prevIrRaw = ir;

    float irIn = ir - irDc;
    irDc += DC_ALPHA * irIn;
    float redIn = red - redDc;
    redDc += DC_ALPHA * redIn;

    float irF = filter_process(&irFilter, irIn);
    float redF = filter_process(&redFilter, redIn);

    /* Motion cancelling: everything below (envelope, edges, autocorrelation)
     * sees the cleaned signal. */
    float irBefore = irF;

    if (mcEnabled && acc != NULL) {
        bool adapt = sampleIdx >= holdoffUntil;
        float irC;

        mc_push_ref(acc);
        irC = mc_cancel(&irNlms, irF, adapt);
        redF = mc_cancel(&redNlms, redF, adapt);
        mcInPow += 0.01f * (irF * irF - mcInPow);
        mcOutPow += 0.01f * (irC * irC - mcOutPow);
        irF = irC;
    }
    if (motion_available()) {
        mc_record(irBefore, irF);
    }

    redSlopeAvg = 0.7f * redSlopeAvg + 0.3f * (redF - redPrevFiltered);

    if (sampleIdx >= holdoffUntil) {
        envAbs += ENV_ALPHA * (fabsf(irF) - envAbs);

        decAcc += irF;
        if (++decCount >= WIN_DECIM) { /* one window sample per WIN_DECIM input samples */
            winBuf[winHead] = decAcc / WIN_DECIM;
            winHead = (winHead + 1) % WIN_LEN;
            if (winCount < WIN_LEN) {
                winCount++;
            }
            decAcc = 0;
            decCount = 0;
        }

        bool signalOk = irDc > 0 && (envAbs / irDc) >= MIN_ACDC && irRaw < SAT_LEVEL;
        bool fallingEdge = (irPrevFiltered >= 0) && (irF < 0) &&
                           ((irPrevFiltered - irF) > SLOPE_GATE * envAbs);
        if (fallingEdge && signalOk) {
            onEdge();
        }
    }

    irPrevFiltered = irF;
    redPrevFiltered = redF;

    if (sampleIdx - lastTickSample >= (uint32_t)fsHz) {
        lastTickSample = sampleIdx;
        onTick(irRaw);
    }
}

static void startSession(void)
{
    filter_reset(&irFilter);
    filter_reset(&redFilter);
    irPrevFiltered = redPrevFiltered = redSlopeAvg = 0;
    envAbs = 0;
    fsHz = FS_NOMINAL;
    sampleIdx = 0;
    lastTickSample = 0;
    holdoffUntil = 0;
    needInit = true;
    winHead = winCount = 0;
    decAcc = 0;
    decCount = 0;
    lastR = 0;
    failTicks = 0;
    acUs = 0;
    pendingCount = 0;
    hrHistCount = hrHistSpot = 0;
    hrValid = false;
    hrEstimate = 0;
    haveLastBeat = false;
    beatsCollected = 0;
    rateSpot = 0;
    beatMedian = 0;
    mc_reset(); /* a new LED/session means a different motion-to-PPG coupling */
    sessionStartMs = k_uptime_get();
    LOG_INF("SESSION START - warming up ~10s (2s settle + 8s window), keep finger still");
}

/* ============================================================================
 * Sensor I/O
 * ========================================================================== */

static int ppg_init(void)
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

    LOG_INF("MAX30101 init succeeded (%s only, LED 0x%02X, falling-edge + sliding autocorrelation)",
            led_name(ppgLed), ledCurrent);
    mcEnabled = motion_available(); /* the same check the boot banner reports */
    startSession();
    return 0;
}

/* Read every sample waiting in the FIFO (like SparkFun's check()) and run each
 * one through the pipeline. */
static void ppg_poll(void)
{
    static float accPrev[MC_AXES];
    static bool accPrevOk;
    float accNow[MC_AXES];
    uint8_t wr, rd;
    int pending;

    /* One IMU reading per poll, taken just before the FIFO read so it lines up
     * with the newest PPG sample; the samples in between are interpolated from
     * the previous poll's reading (polls are 50 ms apart, motion is < 4 Hz). */
    bool accOk = mcEnabled && motion_read_accel(accNow) == 0;

    if (i2c_reg_read_byte_dt(&ppg_i2c, MAX3010X_REG_FIFO_WR_PTR, &wr) < 0 ||
        i2c_reg_read_byte_dt(&ppg_i2c, MAX3010X_REG_FIFO_RD_PTR, &rd) < 0) {
        LOG_ERR("Failed to read MAX30101 FIFO pointers");
        return;
    }

    /* WR == RD is treated as empty. OVF_COUNTER reads nonzero on this board even
     * while every sample is being drained, so it can't tell "full" from "empty". */
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

        /* As on the ESP32: the single LED is fed in as both channels. */
        float acc[MC_AXES];
        const float *accp = NULL;

        if (accOk) {
            float t = (float)(i + 1) / (float)pending;

            for (int a = 0; a < MC_AXES; a++) {
                acc[a] = accPrevOk ? accPrev[a] + (accNow[a] - accPrev[a]) * t : accNow[a];
            }
            accp = acc;
        }

        sendRawSample(sample.val1, sample.val1);
        processSample(sample.val1, sample.val1, accp);
    }

    if (accOk) {
        memcpy(accPrev, accNow, sizeof(accPrev));
    }
    accPrevOk = accOk;

    bench_poll_rx();

    heart_rate_valid = hrValid;
    latest_heart_rate = hrValid ? (int32_t)(hrEstimate + 0.5f) : 0;
}

#endif /* HR_MAIN_C */
