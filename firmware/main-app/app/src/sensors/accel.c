/*
 * Accelerometer wrapper. With the INT pin routed, the watermark interrupt
 * posts EVT_ACCEL_WM; otherwise the FIFO is drained in the PPG pass.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/core/acq.h"
#include "app/counters.h"
#include "app/drv/accel_drv.h"
#include "app/events.h"
#include "app/sensors/sensors.h"

LOG_MODULE_REGISTER(accel, CONFIG_APP_LOG_LEVEL);

#define ACCEL_RANGE_G   4
/* With the INT routed: wake at ~1 s of data. Without it the FIFO must hold
 * >= 1.5 s, which any candidate part does at 25 Hz.
 */
#define ACCEL_WATERMARK 25

static const struct accel_drv_cfg cfg = {
	.rate_hz = CONFIG_APP_ACCEL_RATE_HZ,
	.range_g = ACCEL_RANGE_G,
	.watermark = ACCEL_WATERMARK,
};

/* ISR context. */
static void accel_wm_isr(void)
{
	acq_post(EVT_ACCEL_WM);
}

int accel_init(void)
{
	return accel_drv_init(&cfg, IS_ENABLED(CONFIG_APP_ACCEL_INT_ROUTED) ? accel_wm_isr : NULL);
}

int accel_start(void)
{
	int err = accel_drv_start();

	health_report(SENSOR_ACCEL, err);
	return err;
}

int accel_stop(void)
{
	return accel_drv_stop();
}

int accel_read(struct ppg_block *blk)
{
	bool ovf = false;
	uint8_t n = 0;
	int err = accel_drv_fifo_drain(blk->accel, ACCEL_BLOCK_MAX_SAMPLES, &n, &ovf);

	health_report(SENSOR_ACCEL, err);
	if (err) {
		return err;
	}
	blk->accel_n = n;
	blk->accel_rate_hz = cfg.rate_hz;
	if (ovf) {
		blk->flags |= PPG_BLK_ACCEL_OVF;
		counter_inc(CNT_ACCEL_CHIP_OVF);
	}
	return 0;
}

int accel_reinit(void)
{
	(void)accel_stop();
	int err = accel_drv_reset();

	if (err == 0) {
		err = accel_start();
	}
	return err;
}
