#ifndef HR_MAIN_C
#define HR_MAIN_C

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "spo2_algorithm.h"

#define MAXIM_HR_SAMPLE_COUNT       100
#define MAX30102_HR_UPDATE_SAMPLES  25
#define MAX30102_SAMPLE_PERIOD_MS   40 /* 25Hz sampling rate */

static const struct device *ppg_sensor = DEVICE_DT_GET_ANY(maxim_max30101);

static uint32_t red_buffer[MAXIM_HR_SAMPLE_COUNT];
static uint32_t ir_buffer[MAXIM_HR_SAMPLE_COUNT];
static size_t samples_in_window = 0;

static int32_t latest_heart_rate = 0;
static int8_t heart_rate_valid = 0;
static int32_t latest_spo2 = 0;
static int8_t spo2_valid = 0;

static int max30102_try_init_sensor(void)
{
    if (!device_is_ready(ppg_sensor)) {
        LOG_ERR("Zephyr MAX30101 driver not ready. Check Devicetree I2C setup.");
        return -ENODEV;
    }

    LOG_INF("Zephyr MAX30101 initialized. Place finger on sensor.");
    return 0;
}

static void max30102_update_measurements(void)
{
    maxim_heart_rate_and_oxygen_saturation(
        ir_buffer, MAXIM_HR_SAMPLE_COUNT, red_buffer,
        &latest_spo2, &spo2_valid,
        &latest_heart_rate, &heart_rate_valid
    );

    /* Capture the exact raw IR value being evaluated */
    uint32_t current_ir = ir_buffer[MAXIM_HR_SAMPLE_COUNT - 1];

    if (current_ir < 50000) {
        LOG_WRN("No finger detected. Raw IR: %u", current_ir);
        heart_rate_valid = 0;
        spo2_valid = 0;
    } else {
        LOG_INF("Zephyr Driver Alg -> HR=%d (valid=%d) SPO2=%d (valid=%d)", 
                latest_heart_rate, heart_rate_valid, latest_spo2, spo2_valid);
    }
}

static void max30102_fetch_single_sample(void)
{
    struct sensor_value red, ir;

    /* Zephyr driver handles all I2C transactions here */
    if (sensor_sample_fetch(ppg_sensor) < 0) {
        LOG_ERR("Failed to fetch MAX30102 sample");
        return;
    }

    sensor_channel_get(ppg_sensor, SENSOR_CHAN_RED, &red);
    sensor_channel_get(ppg_sensor, SENSOR_CHAN_IR, &ir);

    /* val1 contains the raw ADC count in the Zephyr driver */
    red_buffer[samples_in_window] = (uint32_t)red.val1;
    ir_buffer[samples_in_window] = (uint32_t)ir.val1;
    samples_in_window++;

    if (samples_in_window == MAXIM_HR_SAMPLE_COUNT) {
        max30102_update_measurements();
        
        /* Slide the window */
        memmove(red_buffer, red_buffer + MAX30102_HR_UPDATE_SAMPLES,
                (MAXIM_HR_SAMPLE_COUNT - MAX30102_HR_UPDATE_SAMPLES) * sizeof(uint32_t));
        memmove(ir_buffer, ir_buffer + MAX30102_HR_UPDATE_SAMPLES,
                (MAXIM_HR_SAMPLE_COUNT - MAX30102_HR_UPDATE_SAMPLES) * sizeof(uint32_t));
        
        samples_in_window = MAXIM_HR_SAMPLE_COUNT - MAX30102_HR_UPDATE_SAMPLES;
    }
}

#endif /* HR_MAIN_C */