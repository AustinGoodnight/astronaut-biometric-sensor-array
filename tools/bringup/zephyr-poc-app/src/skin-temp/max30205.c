/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/logging/log.h>

#include "max30205.h"

LOG_MODULE_REGISTER(max30205, LOG_LEVEL_INF);

#define MAX30205_REG_TEMP       0x00
#define MAX30205_REG_CONFIG     0x01
#define MAX30205_REG_THYST      0x02
#define MAX30205_REG_TOS        0x03

#define MAX30205_CONFIG_CONTINUOUS 0x00
#define MAX30205_THYST_DEFAULT     0x4B00
#define MAX30205_TOS_DEFAULT       0x5000

static struct i2c_dt_spec sensor;
static bool sensor_initialized;

static int read_register_16(uint8_t reg, uint16_t *value)
{
	uint8_t bytes[2];
	int ret;

	ret = i2c_burst_read_dt(&sensor, reg, bytes, sizeof(bytes));
	if (ret < 0) {
		return ret;
	}

	*value = ((uint16_t)bytes[0] << 8) | bytes[1];
	return 0;
}

int max30205_init(void)
{
	uint16_t thyst;
	uint16_t tos;
	int ret;

	sensor_initialized = false;
	sensor = (struct i2c_dt_spec)I2C_DT_SPEC_GET(DT_NODELABEL(skin_temp));

	if (!i2c_is_ready_dt(&sensor)) {
		LOG_ERR("I2C bus for MAX30205 is not ready");
		return -ENODEV;
	}

	ret = read_register_16(MAX30205_REG_THYST, &thyst);
	if (ret < 0) {
		LOG_ERR("Failed to read MAX30205 THYST register: %d", ret);
		return ret;
	}

	ret = read_register_16(MAX30205_REG_TOS, &tos);
	if (ret < 0) {
		LOG_ERR("Failed to read MAX30205 TOS register: %d", ret);
		return ret;
	}

	if (thyst != MAX30205_THYST_DEFAULT || tos != MAX30205_TOS_DEFAULT) {
		LOG_WRN("MAX30205 threshold registers differ from power-on defaults "
			"(THYST=0x%04x, TOS=0x%04x); continuing",
			thyst, tos);
	}

	ret = i2c_reg_write_byte_dt(&sensor, MAX30205_REG_CONFIG,
				    MAX30205_CONFIG_CONTINUOUS);
	if (ret < 0) {
		LOG_ERR("Failed to configure MAX30205 for continuous conversion: %d",
			ret);
		return ret;
	}

	sensor_initialized = true;
	LOG_INF("MAX30205 initialized at I2C address 0x%02x", sensor.addr);
	return 0;
}

int max30205_read_temp_millicelsius(int32_t *temperature_millicelsius)
{
	uint16_t raw;
	int32_t signed_raw;
	int ret;

	if (temperature_millicelsius == NULL) {
		return -EINVAL;
	}

	if (!sensor_initialized) {
		return -EAGAIN;
	}

	if (!i2c_is_ready_dt(&sensor)) {
		return -ENODEV;
	}

	ret = read_register_16(MAX30205_REG_TEMP, &raw);
	if (ret < 0) {
		LOG_ERR("Failed to read MAX30205 temperature register: %d", ret);
		return ret;
	}

	signed_raw = (raw & 0x8000U) ? (int32_t)raw - 0x10000 : (int32_t)raw;
	*temperature_millicelsius = (signed_raw * 1000) / 256;

	return 0;
}
