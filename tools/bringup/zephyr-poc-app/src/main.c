#include <stdint.h>
#include <stdbool.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "persp/persp.h"
#include "max/hr_main.c"

LOG_MODULE_REGISTER(blinky_main, LOG_LEVEL_INF);

struct sensor_data {
    int hr;
    int resp_rate;
    int skin_resistance_kohms;
    float skin_temp;
    float gps_coord;
    int steps;
};
static struct sensor_data sd1;

struct k_work_delayable sensor_poll;
static struct k_work_delayable max30102_sample_poll;

int respiration_handler(){return 20;}
float stemp_handler(){return 95.4;}
float gps_handler(){return 0.0;}
int steps_handler(){return 145;}

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
    return true; 
}

static bool data_binary_encoding()
{
    uint8_t payload[11] = {0};
    
    pack_bits(payload, sizeof(payload) * 8, 1, 9, (uint32_t)sd1.hr);
    pack_bits(payload, sizeof(payload) * 8, 10, 8, (uint32_t)sd1.resp_rate);
    pack_bits(payload, sizeof(payload) * 8, 18, 11,(uint32_t)sd1.skin_resistance_kohms);
    pack_bits(payload, sizeof(payload) * 8, 29, 11, (uint32_t)(sd1.skin_temp * 10));
    pack_bits(payload, sizeof(payload) * 8, 40, 32, (uint32_t)(sd1.gps_coord * 10));
    pack_bits(payload, sizeof(payload) * 8, 72, 9, (uint32_t)sd1.steps);

    blt_transmit(payload, sizeof(payload));
    return true;
}

static void max30102_sample_poll_handler(struct k_work *work)
{
    ARG_UNUSED(work);
    
    max30102_fetch_single_sample();

    k_work_schedule(&max30102_sample_poll, K_MSEC(MAX30102_SAMPLE_PERIOD_MS));
}

static void sensor_polling_handler(struct k_work *work)
{
    uint16_t resistance_kohms;
    int ret;

    ARG_UNUSED(work);

    ret = persp_read_resistance_kohms(&resistance_kohms);
    if (ret < 0) {
        LOG_ERR("Failed to read skin resistance: %d", ret);
        goto reschedule;
    }

    sd1.hr = heart_rate_valid ? latest_heart_rate : 0;
    sd1.resp_rate = respiration_handler();
    sd1.skin_resistance_kohms = resistance_kohms;
    sd1.skin_temp = stemp_handler();
    sd1.gps_coord = gps_handler();
    sd1.steps = steps_handler();

    LOG_INF("Sensor Data - HR: %d, Resp: %d, Resistance: %d kOhm, Temp: %.1f, GPS: %.1f",
            sd1.hr, sd1.resp_rate, sd1.skin_resistance_kohms,
            (double)sd1.skin_temp, (double)sd1.gps_coord);

    data_binary_encoding();

    reschedule:
    k_work_schedule(&sensor_poll, K_MSEC(2000));
}

int main(void)
{
    int ret = persp_init();

    if (ret < 0) {
        LOG_ERR("Failed to initialize skin-resistance sensor: %d", ret);
        return ret;
    }

    ret = max30102_try_init_sensor();
    if (ret < 0) {
        return ret;
    }

    k_work_init_delayable(&max30102_sample_poll, max30102_sample_poll_handler);
    k_work_schedule(&max30102_sample_poll, K_MSEC(MAX30102_SAMPLE_PERIOD_MS));

    k_work_init_delayable(&sensor_poll, sensor_polling_handler);
    k_work_schedule(&sensor_poll, K_MSEC(2000));
    
    return 0;
}