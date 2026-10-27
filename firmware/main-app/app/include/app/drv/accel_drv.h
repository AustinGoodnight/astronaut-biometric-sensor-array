/*
 * Accelerometer driver interface. The part is an open decision
 * (ARCHITECTURE.md §13 #1). Whatever is chosen must provide a FIFO with a
 * watermark; the interrupt pin is optional (CONFIG_APP_ACCEL_INT_ROUTED).
 *
 * Implemented by src/drivers/accel_<part>.c (only accel_none.c so far) and
 * src/sim/fake_sensors.c. acq thread only, except the callback (ISR).
 *
 * Return codes: 0; -EIO bus error; -ENODEV not present; -ENOSYS stub.
 */

#ifndef APP_DRV_ACCEL_DRV_H_
#define APP_DRV_ACCEL_DRV_H_

#include <stdbool.h>
#include <stdint.h>

struct accel_drv_cfg {
	uint16_t rate_hz;
	/** Full-scale range in g (2, 4, 8, 16). */
	uint8_t range_g;
	/** FIFO watermark in samples (only matters with the INT routed). */
	uint8_t watermark;
};

/** ISR context: post EVT_ACCEL_WM and return. */
typedef void (*accel_drv_wm_cb_t)(void);

/**
 * @brief Probe and configure; leave the part in standby.
 *
 * @param cb NULL if the INT pin is not routed (drain-on-PPG mode).
 * acq/main init path; blocks on I2C.
 */
int accel_drv_init(const struct accel_drv_cfg *cfg, accel_drv_wm_cb_t cb);

/** @brief Start sampling into the FIFO. acq; blocks on I2C. */
int accel_drv_start(void);

/** @brief Stop sampling (low-power standby), INT disabled. acq; blocks on I2C. */
int accel_drv_stop(void);

/**
 * @brief Drain the FIFO.
 *
 * acq; blocks on I2C.
 * @param samples  Output x, y, z in mg.
 * @param max      Capacity (samples).
 * @param n        Out: samples read.
 * @param overflow Out: FIFO overflowed since the last drain.
 */
int accel_drv_fifo_drain(int16_t samples[][3], uint8_t max, uint8_t *n, bool *overflow);

/** @brief Soft-reset and reconfigure (recovery step). Leaves it stopped. acq; blocks. */
int accel_drv_reset(void);

#endif /* APP_DRV_ACCEL_DRV_H_ */
