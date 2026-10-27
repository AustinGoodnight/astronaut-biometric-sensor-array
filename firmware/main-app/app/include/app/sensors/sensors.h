/*
 * App-facing sensor wrappers (src/sensors/<name>.c). Called from the acq thread
 * only. Each wrapper owns its sensor's power sequencing (boost, front-end
 * enables), converts to the units proc expects, and reports every
 * operation's result to health.
 *
 * Power-sequencing rule: disable a sensor's interrupt BEFORE cutting its
 * power, and re-enable it only after re-init.
 *
 * Return codes: 0 or negative errno from the driver layer (already reported
 * to health; callers just skip the data).
 */

#ifndef APP_SENSORS_SENSORS_H_
#define APP_SENSORS_SENSORS_H_

#include <stdbool.h>
#include <stdint.h>

#include "app/core/health.h"
#include "app/ppg_block.h"

/* --- PPG (MAX30101 + 5 V boost) --- */

/** @brief Probe and configure (stopped). Init path. */
int ppg_init(void);
/** @brief Boost on, chip out of shutdown, INT on. */
int ppg_start(void);
/** @brief INT off, chip shutdown, boost off. */
int ppg_stop(void);
/** @brief Burst-read the FIFO into @p blk (sets ppg_n, channels, rate, overflow flag). */
int ppg_read(struct ppg_block *blk);
/** @brief Recovery: stop, soft reset, reconfigure, start. */
int ppg_reinit(void);

/* --- Accelerometer --- */

int accel_init(void);
int accel_start(void);
int accel_stop(void);
/** @brief Drain the FIFO into @p blk (sets accel_n, rate, overflow flag). */
int accel_read(struct ppg_block *blk);
int accel_reinit(void);

/* --- Skin temperature --- */

int skin_temp_init(void);
/** @brief @p mdegc: previous conversion (see temp_drv_read()). -EAGAIN = none yet. */
int skin_temp_read(int32_t *mdegc);
int skin_temp_reinit(void);

/* --- GSR (front end + SAADC) --- */

int gsr_init(void);
/** @brief Front end on (phase 1). Never fails. */
void gsr_power(bool on);
/** @brief SAADC read in mV (phase 2, after the settle time). */
int gsr_read(int32_t *mv);

/* --- Flex respiration sensor (divider + SAADC) --- */

int flex_init(void);
void flex_power(bool on);
int flex_read(int32_t *mv);

/* --- Fuel gauge --- */

/** @brief Probe; set the low-battery alert. Init path. */
int fuel_gauge_init(void);
/** @brief SOC in %. */
int fuel_gauge_read(uint8_t *soc_pct);
int fuel_gauge_reinit(void);

/**
 * @brief Re-init one sensor (recovery step RECOVERY_REINIT).
 *
 * acq only. Reports the result to health.
 */
int sensors_reinit(enum sensor_id sensor);

#endif /* APP_SENSORS_SENSORS_H_ */
