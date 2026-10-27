/*
 * MAX17048 fuel gauge thin driver (implements fuel_gauge_drv.h). STUB.
 *
 * Why not the in-tree fuel_gauge driver: no ALRT configuration, and we want
 * raw register access for bring-up (the last charger board's gauges never
 * ACKed, ARCHITECTURE.md §5).
 *
 * TODO:
 *  init:  i2c_is_ready_dt; read VERSION (0x08, expect 0x001x) else -ENODEV;
 *         CONFIG (0x0C): ATHD = 32 - alert_pct, clear ALRT; set up
 *         alert-gpios (if present) with an edge-to-active interrupt -> cb.
 *  read:  VCELL (0x02): 78.125 µV/LSB -> mV; SOC (0x04): high byte = %.
 *  clear_alert: CONFIG.ALRT = 0; STATUS (0x1A) bits cleared.
 *  reset: CMD (0xFE) = 0x5400 (power-on reset), then re-run init.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "app/drv/fuel_gauge_drv.h"

#define FG_NODE DT_NODELABEL(fuel_gauge)

#define MAX17048_REG_VCELL   0x02
#define MAX17048_REG_SOC     0x04
#define MAX17048_REG_VERSION 0x08
#define MAX17048_REG_CONFIG  0x0C
#define MAX17048_REG_STATUS  0x1A
#define MAX17048_REG_CMD     0xFE

static const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(FG_NODE);
static const struct gpio_dt_spec alert = GPIO_DT_SPEC_GET_OR(FG_NODE, alert_gpios, {0});

static fg_drv_alert_cb_t alert_cb;

int fg_drv_init(uint8_t alert_pct, fg_drv_alert_cb_t cb)
{
	ARG_UNUSED(alert_pct);
	alert_cb = cb;

	if (!i2c_is_ready_dt(&i2c)) {
		return -ENODEV;
	}
	if (alert.port != NULL && !gpio_is_ready_dt(&alert)) {
		return -ENODEV;
	}
	return -ENOSYS;
}

int fg_drv_read(struct fg_reading *out)
{
	ARG_UNUSED(out);
	return -ENOSYS;
}

int fg_drv_clear_alert(void)
{
	return -ENOSYS;
}

int fg_drv_reset(void)
{
	return -ENOSYS;
}
