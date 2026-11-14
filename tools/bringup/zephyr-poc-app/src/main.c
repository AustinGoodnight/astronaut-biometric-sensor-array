#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

#include "persp/persp.h"
#include "ble/ble.h"
#include "motion/motion.h"
/* Must come before hr_main.c: it is #included here and uses LOG_*. */
LOG_MODULE_REGISTER(blinky_main, LOG_LEVEL_INF);

#include "max/hr_main.c"

struct sensor_data {
    int hr;
    int resp_rate;
    int skin_resistance_kohms;
    float skin_temp;
    int32_t lat_e7;
    int32_t lon_e7;
    int steps;
    uint8_t active;   /* BIT(ACT_*) set = that field comes from a working sensor */
};
static struct sensor_data sd1;

struct k_work_delayable sensor_poll;
static struct k_work_delayable ppg_poll_work;

int respiration_handler(){return 20;}
float stemp_handler(){return 95.4;}
int32_t gps_lat_e7_handler(){return 0;}   /* degrees x 10^7, signed */
int32_t gps_lon_e7_handler(){return 0;}
int steps_handler(){return 145;}

/* BLE payload layout: bit offset / width, MSB-first, bit 0 reserved.
 * 120 bits -> 15 bytes. The dashboard decoder mirrors this table. */
#define PAYLOAD_LEN  15
#define F_HR_OFF      1
#define F_HR_W        9    /* bpm, 0 = no pulse */
#define F_RESP_OFF   10
#define F_RESP_W      8    /* breaths/min */
#define F_RES_OFF    18
#define F_RES_W      11    /* skin resistance kOhm, 0 = no reading */
#define F_TEMP_OFF   29
#define F_TEMP_W     11    /* skin temp degF x 10 */
#define F_LAT_OFF    40
#define F_LAT_W      32    /* degrees x 10^7, two's complement */
#define F_LON_OFF    72
#define F_LON_W      32    /* degrees x 10^7, two's complement */
#define F_STEPS_OFF 104
#define F_STEPS_W     9
/* Bits 113-118: one "sensor active" flag per field, so a 0 reading can be
 * told apart from "no sensor". Bit 119 is spare. A field whose flag is clear
 * should be ignored by the receiver. */
#define F_ACTIVE_OFF 113
enum { ACT_HR, ACT_RESP, ACT_RES, ACT_TEMP, ACT_GPS, ACT_STEPS, ACT_COUNT };

static bool hr_sensor_ok;

/* Clamp an unsigned field to [0, 2^width - 1] so an out-of-range value is
 * sent as the field maximum instead of being rejected by pack_bits() (which
 * would leave it 0, indistinguishable from "no reading"). */
static uint32_t fit(int value, unsigned width)
{
    return (uint32_t)CLAMP(value, 0, (int)((1U << width) - 1U));
}

static bool pack_bits(uint8_t *buf, size_t buf_bits, size_t offset, unsigned width, uint32_t value)
{
    if (width == 0 || width > 32 || offset > buf_bits || width > buf_bits - offset || (width < 32 && value >= (UINT32_C(1) << width))) {
        return false;
    }
    for (unsigned i = 0; i < width; ++i) {
        size_t bit = offset + i;
        unsigned value_bit = width - 1 - i;

        if ((value >> value_bit) & 1U) {
            buf[bit / 8] |= (uint8_t)(1U << (7 - bit % 8));
        }
    }
    return true;
}

static bool blt_transmit(uint8_t *data, size_t len)
{
    LOG_HEXDUMP_INF(data, len, "Payload");
    return ble_send(data, (uint16_t)len) == 0;
}

static bool data_binary_encoding(void)
{
    uint8_t payload[PAYLOAD_LEN] = {0};
    const size_t bits = sizeof(payload) * 8;
    int temp_x10 = (int)(sd1.skin_temp * 10.0f + 0.5f);   /* round, don't truncate */

    pack_bits(payload, bits, F_HR_OFF,    F_HR_W,    fit(sd1.hr, F_HR_W));
    pack_bits(payload, bits, F_RESP_OFF,  F_RESP_W,  fit(sd1.resp_rate, F_RESP_W));
    pack_bits(payload, bits, F_RES_OFF,   F_RES_W,   fit(sd1.skin_resistance_kohms, F_RES_W));
    pack_bits(payload, bits, F_TEMP_OFF,  F_TEMP_W,  fit(temp_x10, F_TEMP_W));
    /* Signed -> unsigned keeps the two's-complement bits (well defined in C). */
    pack_bits(payload, bits, F_LAT_OFF,   F_LAT_W,   (uint32_t)sd1.lat_e7);
    pack_bits(payload, bits, F_LON_OFF,   F_LON_W,   (uint32_t)sd1.lon_e7);
    pack_bits(payload, bits, F_STEPS_OFF, F_STEPS_W, fit(sd1.steps, F_STEPS_W));
    for (unsigned i = 0; i < ACT_COUNT; i++) {
        pack_bits(payload, bits, F_ACTIVE_OFF + i, 1, (sd1.active >> i) & 1U);
    }

    return blt_transmit(payload, sizeof(payload));
}

static void ppg_poll_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    
    ppg_poll();

    k_work_schedule(&ppg_poll_work, K_MSEC(PPG_POLL_PERIOD_MS));
}

static void sensor_polling_handler(struct k_work *work)
{
    uint16_t resistance_kohms;
    int ret;

    ARG_UNUSED(work);

    /* Respiration, temperature, GPS and steps are placeholders, not sensors,
     * so their flags stay clear. */
    sd1.active = 0;
    if (hr_sensor_ok) {
        sd1.active |= BIT(ACT_HR);
    }

    ret = persp_read_resistance_kohms(&resistance_kohms);
    if (ret < 0) {
        LOG_ERR("Failed to read skin resistance: %d", ret);
        resistance_kohms = 0;
    } else {
        sd1.active |= BIT(ACT_RES);
    }

    sd1.hr = heart_rate_valid ? latest_heart_rate : 0;
    sd1.resp_rate = respiration_handler();
    sd1.skin_resistance_kohms = resistance_kohms;
    sd1.skin_temp = stemp_handler();
    sd1.lat_e7 = gps_lat_e7_handler();
    sd1.lon_e7 = gps_lon_e7_handler();
    sd1.steps = steps_handler();

    LOG_INF("Sensor Data - HR: %d, Resp: %d, Resistance: %d kOhm, Temp: %.1f, Lat: %d, Lon: %d, active: 0x%02x",
            sd1.hr, sd1.resp_rate, sd1.skin_resistance_kohms,
            (double)sd1.skin_temp, (int)sd1.lat_e7, (int)sd1.lon_e7, sd1.active);

    data_binary_encoding();

    k_work_schedule(&sensor_poll, K_MSEC(2000));
}

/* Boot logs go out over USB CDC, which drops output until a terminal opens
 * the port. Wait up to 5 s for one (DTR set) so boot errors aren't lost. */
static void wait_for_usb_console(void)
{
    const struct device *console = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
    uint32_t dtr = 0;

    for (int i = 0; i < 50 && !dtr; i++) {
        uart_line_ctrl_get(console, UART_LINE_CTRL_DTR, &dtr);
        k_msleep(100);
    }
}

int main(void)
{
    wait_for_usb_console();
    LOG_INF("Wrist POC booting");

    /* Say which build this is, so a serial log shows it at a glance. */
#ifdef CONFIG_LSM6DSL
    /* hr_main.c turns its adaptive filter on from this same result
     * (motion_available()), so the banner matches what actually runs. */
    LOG_INF("Board: XIAO Sense build, motion cancel %s",
            motion_init() ? "ON (adaptive filter)" : "OFF (IMU not found)");
#else
    LOG_INF("Board: XIAO build, motion cancel OFF (no IMU)");
#endif

    /* Bluetooth failing shouldn't stop the sensors; the USB log still works. */
    LOG_INF("Enabling Bluetooth...");
    if (ble_init() < 0) {
        LOG_ERR("Bluetooth unavailable, continuing without it");
    }

    int ret = persp_init();

    if (ret < 0) {
        LOG_WRN("Failed to initialize skin-resistance sensor: %d; continuing without it",
                ret);
    }

    /* No heart-rate sensor shouldn't stop telemetry; HR is sent as 0. */
    hr_sensor_ok = (ppg_init() == 0);
    if (hr_sensor_ok) {
        k_work_init_delayable(&ppg_poll_work, ppg_poll_handler);
        k_work_schedule(&ppg_poll_work, K_MSEC(PPG_POLL_PERIOD_MS));
    } else {
        LOG_ERR("Continuing without heart rate");
    }

    k_work_init_delayable(&sensor_poll, sensor_polling_handler);
    k_work_schedule(&sensor_poll, K_MSEC(2000));
    
    return 0;
}