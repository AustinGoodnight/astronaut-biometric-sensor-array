/*
 * Fake sensors (CONFIG_APP_FAKE_SENSORS): generators behind the
 * include/app/drv/<name>.h interfaces, plus fault injection for testing the
 * health state machine and validity flags (ARCHITECTURE.md §11).
 *
 * Shell (debug builds):
 *   fake fault <sensor> <none|nack|flatline|range> [count]
 *   fake hr <bpm>
 *   fake motion <0|1>
 */

#ifndef APP_SIM_FAKE_SENSORS_H_
#define APP_SIM_FAKE_SENSORS_H_

#include <stdbool.h>
#include <stdint.h>

#include "app/core/health.h"

enum fake_fault {
	FAKE_FAULT_NONE = 0,
	/** Every op on the sensor returns -EIO (exercises health escalation). */
	FAKE_FAULT_NACK,
	/** Data stays constant (exercises *_validate quality checks). */
	FAKE_FAULT_FLATLINE,
	/** Data saturates / leaves physiological bounds. */
	FAKE_FAULT_RANGE,
};

/**
 * @brief Inject a fault.
 *
 * @param count Ops until the fault clears by itself; 0 = until set to NONE.
 * Any thread; never blocks.
 */
void fake_sensors_set_fault(enum sensor_id sensor, enum fake_fault fault, uint32_t count);

/** @brief Simulated heart rate. Any thread. */
void fake_sensors_set_hr(uint16_t bpm);

/** @brief Simulated wrist motion on/off. Any thread. */
void fake_sensors_set_motion(bool on);

/** @brief Fake I2C bus recovery (called by board_bus_recover()). acq. */
int fake_sensors_bus_recover(enum app_bus bus);

#endif /* APP_SIM_FAKE_SENSORS_H_ */
