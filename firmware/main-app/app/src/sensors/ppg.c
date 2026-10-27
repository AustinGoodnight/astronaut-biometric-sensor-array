/*
 * PPG wrapper: MAX30101 driver + 5 V boost for the LEDs. The boost is on
 * only between ppg_start() and ppg_stop().
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/core/acq.h"
#include "app/counters.h"
#include "app/drv/ppg_drv.h"
#include "app/events.h"
#include "app/sensors/board.h"
#include "app/sensors/sensors.h"

LOG_MODULE_REGISTER(ppg, CONFIG_APP_LOG_LEVEL);

#define PPG_CHANNELS          1   /* green only for now; IR for off-wrist is a TODO */
#define PPG_LED_CURRENT_MA10  100 /* 10.0 mA; tune on the bench (ARCHITECTURE.md §7.1) */

static const struct ppg_drv_cfg cfg = {
	.rate_hz = CONFIG_APP_PPG_RATE_HZ,
	.fifo_threshold = CONFIG_APP_PPG_FIFO_THRESHOLD,
	.channels = PPG_CHANNELS,
	.led_current_ma10 = PPG_LED_CURRENT_MA10,
};

/* ISR context. */
static void ppg_fifo_isr(void)
{
	acq_post(EVT_PPG_FIFO);
}

int ppg_init(void)
{
	return ppg_drv_init(&cfg, ppg_fifo_isr);
}

int ppg_start(void)
{
	(void)board_boost_set(true);
	/* TODO: wait for the boost output to settle (datasheet) if the first
	 * samples after start turn out to be low.
	 */
	int err = ppg_drv_start();

	health_report(SENSOR_PPG, err);
	if (err) {
		(void)board_boost_set(false);
	}
	return err;
}

int ppg_stop(void)
{
	int err = ppg_drv_stop(); /* INT disabled first, inside the driver */

	(void)board_boost_set(false);
	return err;
}

int ppg_read(struct ppg_block *blk)
{
	bool ovf = false;
	uint8_t n = 0;
	int err = ppg_drv_fifo_read(blk->ppg, PPG_BLOCK_MAX_SAMPLES, &n, &ovf);

	health_report(SENSOR_PPG, err);
	if (err) {
		return err;
	}
	blk->ppg_n = n;
	blk->ppg_channels = cfg.channels;
	blk->ppg_rate_hz = cfg.rate_hz;
	if (ovf) {
		blk->flags |= PPG_BLK_PPG_OVF;
		counter_inc(CNT_PPG_CHIP_OVF);
	}
	return 0;
}

int ppg_reinit(void)
{
	(void)ppg_stop();
	int err = ppg_drv_reset();

	if (err == 0) {
		err = ppg_start();
	}
	return err;
}
