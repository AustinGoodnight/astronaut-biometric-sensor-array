/*
 * "No accelerometer" (CONFIG_APP_ACCEL_PART_NONE): the part is open
 * decision #1. Reports -ENODEV, so health marks it NOT_PRESENT and blocks
 * carry accel_n = 0. Add drivers/accel_<part>.c behind accel_drv.h when
 * the part is chosen.
 */

#include <zephyr/kernel.h>

#include "app/drv/accel_drv.h"

int accel_drv_init(const struct accel_drv_cfg *cfg, accel_drv_wm_cb_t cb)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(cb);
	return -ENODEV;
}

int accel_drv_start(void)
{
	return -ENODEV;
}

int accel_drv_stop(void)
{
	return 0;
}

int accel_drv_fifo_drain(int16_t samples[][3], uint8_t max, uint8_t *n, bool *overflow)
{
	ARG_UNUSED(samples);
	ARG_UNUSED(max);
	*n = 0;
	*overflow = false;
	return -ENODEV;
}

int accel_drv_reset(void)
{
	return -ENODEV;
}
