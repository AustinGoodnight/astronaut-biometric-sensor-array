/*
 * Acquisition thread (prio 2): the ONLY thread that touches I2C and the
 * SAADC (ARCHITECTURE.md §2). Wakes on k_event bits (events.h), reads
 * sensors, and posts data to proc without ever blocking on it.
 */

#ifndef APP_CORE_ACQ_H_
#define APP_CORE_ACQ_H_

#include <stdint.h>

/**
 * @brief Init the board GPIOs and every sensor; record the init results in health.
 *
 * main, before threads start. May block briefly (sensor probing).
 * @return 0 (sensor failures are recorded in health, not returned).
 */
int acq_init(void);

/** @brief Start the acq thread. main, after all inits. */
void acq_start(void);

/**
 * @brief Post event bits (EVT_*) to the acq thread.
 *
 * Any context, including ISRs and k_timer expiry functions. Never blocks.
 */
void acq_post(uint32_t events);

#endif /* APP_CORE_ACQ_H_ */
