/*
 * MAX30205 / MAX30208 skin temperature (implements temp_drv.h). STUB.
 * Part chosen by CONFIG_APP_SKIN_TEMP_MAX30205 / _MAX30208 (open decision #2).
 *
 * TODO MAX30205 (no in-tree driver):
 *  init:  i2c_is_ready_dt; read CONFIG (0x01) to check it ACKs; set
 *         SHUTDOWN = 1 (one-shot mode, ~0 µA between conversions).
 *  read:  if a conversion was started last call, read TEMP (0x00, 16-bit,
 *         0.00390625 °C/LSB, big-endian) -> m°C; then write CONFIG with
 *         ONE_SHOT | SHUTDOWN to start the next one (~50 ms). First call:
 *         start one, return -EAGAIN.
 *  reset: re-run init.
 *
 * TODO MAX30208 (no in-tree driver): STATUS 0x00, FIFO 0x08..0x0A,
 *  TEMP_SENSOR_SETUP 0x14 (CONVERT_T); read the FIFO the same way.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "app/drv/temp_drv.h"

#define TEMP_NODE DT_NODELABEL(skin_temp)

#if defined(CONFIG_APP_SKIN_TEMP_MAX30205)
#define MAX30205_REG_TEMP     0x00
#define MAX30205_REG_CONFIG   0x01
#define MAX30205_CFG_SHUTDOWN BIT(0)
#define MAX30205_CFG_ONE_SHOT BIT(7)
#else
#define MAX30208_REG_STATUS   0x00
#define MAX30208_REG_FIFO     0x08
#define MAX30208_REG_SETUP    0x14
#endif

static const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(TEMP_NODE);

int temp_drv_init(void)
{
	if (!i2c_is_ready_dt(&i2c)) {
		return -ENODEV;
	}
	return -ENOSYS;
}

int temp_drv_read(int32_t *mdegc)
{
	ARG_UNUSED(mdegc);
	return -ENOSYS;
}

int temp_drv_reset(void)
{
	return temp_drv_init();
}
