/*
 * PPG driver interface (register level, no policy).
 *
 * Implemented by src/drivers/max30101.c (real) and src/sim/fake_sensors.c
 * (CONFIG_APP_FAKE_SENSORS). All functions are called from the acq thread
 * only, except the FIFO callback, which runs in ISR context.
 *
 * Return codes: 0 on success; -EIO on a bus error; -ENODEV if the part
 * didn't answer at init; -ENOSYS for stubbed functionality.
 */

#ifndef APP_DRV_PPG_DRV_H_
#define APP_DRV_PPG_DRV_H_

#include <stdbool.h>
#include <stdint.h>

#include "app/ppg_block.h"

struct ppg_drv_cfg {
	/** Output sample rate after on-chip averaging. */
	uint16_t rate_hz;
	/** FIFO-almost-full interrupt fires when this many samples are waiting. */
	uint8_t fifo_threshold;
	/** Active LED channels (1 = green, 2 = green + IR). */
	uint8_t channels;
	/** Green LED pulse current in 0.1 mA (MAX30101: 0..510). */
	uint16_t led_current_ma10;
};

/** ISR context: post EVT_PPG_FIFO and return. */
typedef void (*ppg_drv_fifo_cb_t)(void);

/**
 * @brief Probe and configure the chip, leave it in shutdown.
 *
 * acq/main init path. Blocks for the I2C transfers. Wires the INT GPIO to @p cb.
 */
int ppg_drv_init(const struct ppg_drv_cfg *cfg, ppg_drv_fifo_cb_t cb);

/** @brief Leave shutdown, clear the FIFO, enable the INT GPIO. acq; blocks on I2C. */
int ppg_drv_start(void);

/** @brief Disable the INT GPIO, then enter shutdown. acq; blocks on I2C. */
int ppg_drv_stop(void);

/**
 * @brief Read every sample waiting in the FIFO in one burst.
 *
 * acq; blocks on I2C (one transfer of up to 32 x channels x 3 bytes).
 *
 * @param samples  Output, [sample][channel], 18-bit raw counts.
 * @param max      Capacity of @p samples (samples).
 * @param n        Out: samples read.
 * @param overflow Out: the chip FIFO overflowed since the last read.
 */
int ppg_drv_fifo_read(uint32_t samples[][PPG_MAX_CHANNELS], uint8_t max, uint8_t *n,
		      bool *overflow);

/**
 * @brief Soft-reset and reconfigure with the init config (recovery step).
 *
 * acq; blocks on I2C. Leaves the chip stopped (call ppg_drv_start()).
 */
int ppg_drv_reset(void);

#endif /* APP_DRV_PPG_DRV_H_ */
