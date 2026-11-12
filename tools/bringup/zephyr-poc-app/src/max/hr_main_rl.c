#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include "maxim_hr.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#define MAX30102_I2C_ADDR 0x57
#define MAX30102_PART_ID_REG 0xFF
#define MAX30102_PART_ID 0x15
#define MAX30102_REG_INT_ENABLE_1 0x02
#define MAX30102_REG_INT_ENABLE_2 0x03
#define MAX30102_REG_FIFO_WRITE_PTR 0x04
#define MAX30102_REG_FIFO_OVERFLOW 0x05
#define MAX30102_REG_FIFO_READ_PTR 0x06
#define MAX30102_REG_FIFO_DATA 0x07
#define MAX30102_REG_FIFO_CONFIG 0x08
#define MAX30102_REG_MODE_CONFIG 0x09
#define MAX30102_REG_SPO2_CONFIG 0x0A
#define MAX30102_REG_LED_RED_PA 0x0C
#define MAX30102_REG_LED_IR_PA 0x0D

#define MAX30102_SAMPLE_PERIOD_MS 10
#define MAX30102_HR_UPDATE_SAMPLES 25
#define MAX30102_NO_FINGER_THRESHOLD 50000

#define MY_STACK_SIZE 512
#define POLLING_PRIORITY 5

static const struct device *const i2c_dev = DEVICE_DT_GET(DT_NODELABEL(xiao_i2c));

static uint32_t ir_buffer[MAXIM_HR_SAMPLE_COUNT];
static uint32_t red_buffer[MAXIM_HR_SAMPLE_COUNT];
static size_t samples_in_window;
static int32_t latest_heart_rate;
static bool heart_rate_valid;

static int max30102_read_register(uint8_t reg, uint8_t *value)
{
    return i2c_reg_read_byte(i2c_dev, MAX30102_I2C_ADDR, reg, value);
}

static int max30102_write_register(uint8_t reg, uint8_t value)
{
    return i2c_reg_write_byte(i2c_dev, MAX30102_I2C_ADDR, reg, value);
}

static int max30102_clear_fifo(void)
{
    int ret;

    ret = max30102_write_register(MAX30102_REG_FIFO_WRITE_PTR, 0);
    if (ret < 0) {
        return ret;
    }

    ret = max30102_write_register(MAX30102_REG_FIFO_OVERFLOW, 0);
    if (ret < 0) {
        return ret;
    }

    return max30102_write_register(MAX30102_REG_FIFO_READ_PTR, 0);
}

static int max30102_try_init_sensor(void)
{
    uint8_t part_id;
    uint8_t mode;
    int ret;

    if (!device_is_ready(i2c_dev)) {
        LOG_ERR("I2C bus device is not ready");
        return -ENODEV;
    }

    ret = i2c_configure(i2c_dev, I2C_MODE_CONTROLLER | I2C_SPEED_SET(I2C_SPEED_FAST));
    if (ret < 0) {
        LOG_ERR("Failed to configure I2C bus for 400 kHz: %d", ret);
        return ret;
    }

    ret = max30102_read_register(MAX30102_PART_ID_REG, &part_id);
    if (ret < 0) {
        LOG_ERR("MAX30102 I2C probe failed: %d", ret);
        return ret;
    }
    if (part_id != MAX30102_PART_ID) {
        LOG_ERR("Unexpected MAX30102 part ID: 0x%02x (expected 0x%02x)",
                part_id, MAX30102_PART_ID);
        return -ENODEV;
    }

    LOG_INF("MAX30102 found (part ID 0x%02x)", part_id);

    ret = max30102_write_register(MAX30102_REG_MODE_CONFIG, 0x40);
    if (ret < 0) {
        LOG_ERR("Failed to reset MAX30102: %d", ret);
        return ret;
    }

    for (int i = 0; i < 100; ++i) {
        k_sleep(K_MSEC(1));
        ret = max30102_read_register(MAX30102_REG_MODE_CONFIG, &mode);
        if (ret < 0) {
            LOG_ERR("Failed while waiting for MAX30102 reset: %d", ret);
            return ret;
        }
        if ((mode & 0x40) == 0) {
            break;
        }
    }
    if ((mode & 0x40) != 0) {
        LOG_ERR("MAX30102 reset timed out");
        return -ETIMEDOUT;
    }

    ret = max30102_write_register(MAX30102_REG_INT_ENABLE_1, 0);
    if (ret < 0) {
        LOG_ERR("Failed to disable MAX30102 interrupts: %d", ret);
        return ret;
    }
    ret = max30102_write_register(MAX30102_REG_INT_ENABLE_2, 0);
    if (ret < 0) {
        LOG_ERR("Failed to disable MAX30102 interrupts: %d", ret);
        return ret;
    }

    /* Match setup(0x3C, 4, 2, 100, 411, 4096) from the Arduino app. */
    ret = max30102_write_register(MAX30102_REG_FIFO_CONFIG, 0x50);
    if (ret < 0) {
        LOG_ERR("Failed to configure MAX30102 FIFO: %d", ret);
        return ret;
    }
    ret = max30102_write_register(MAX30102_REG_MODE_CONFIG, 0x03);
    if (ret < 0) {
        LOG_ERR("Failed to select MAX30102 red/IR mode: %d", ret);
        return ret;
    }
    ret = max30102_write_register(MAX30102_REG_SPO2_CONFIG, 0x27);
    if (ret < 0) {
        LOG_ERR("Failed to configure MAX30102 sample settings: %d", ret);
        return ret;
    }
    ret = max30102_write_register(MAX30102_REG_LED_RED_PA, 0x3C);
    if (ret < 0) {
        LOG_ERR("Failed to set MAX30102 red LED current: %d", ret);
        return ret;
    }
    ret = max30102_write_register(MAX30102_REG_LED_IR_PA, 0x3C);
    if (ret < 0) {
        LOG_ERR("Failed to set MAX30102 IR LED current: %d", ret);
        return ret;
    }
    ret = max30102_clear_fifo();
    if (ret < 0) {
        LOG_ERR("Failed to clear MAX30102 FIFO: %d", ret);
        return ret;
    }

    LOG_INF("MAX30102 initialized; collecting red/IR samples");
    return 0;
}

static void max30102_update_measurements(void)
{
    int8_t hr_valid;
    int32_t heart_rate;
    int ret;

    ret = maxim_calculate_heart_rate(ir_buffer, red_buffer, &heart_rate, &hr_valid);
    if (ret < 0) {
        LOG_ERR("Heart-rate algorithm failed: %d", ret);
        return;
    }

    if (ir_buffer[MAXIM_HR_SAMPLE_COUNT - 1] < MAX30102_NO_FINGER_THRESHOLD) {
        heart_rate_valid = false;
        LOG_INF("No finger detected");
        return;
    }

    heart_rate_valid = (hr_valid != 0);
    if (heart_rate_valid) {
        latest_heart_rate = heart_rate;
        LOG_INF("Heart rate: %d BPM", latest_heart_rate);
    } else {
        LOG_INF("Heart rate estimate not valid yet");
    }
}