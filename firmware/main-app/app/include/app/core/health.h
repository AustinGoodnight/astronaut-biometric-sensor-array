/*
 * Per-sensor health state machine (ARCHITECTURE.md §6.2).
 *
 * acq (and storage, for SENSOR_STORAGE) report every operation's result
 * with health_report(). The health thread runs every second, moves sensors
 * OK -> DEGRADED -> FAILED, picks the next recovery step, and hands it to
 * acq through a one-slot mailbox (health never touches I2C itself: D1).
 */

#ifndef APP_CORE_HEALTH_H_
#define APP_CORE_HEALTH_H_

#include <stdbool.h>
#include <stdint.h>

/** Order = 2-bit slot order in record_data.health. Max 8 entries. */
enum sensor_id {
	SENSOR_PPG = 0,
	SENSOR_ACCEL,
	SENSOR_SKIN_TEMP,
	SENSOR_GSR,
	SENSOR_FLEX,
	SENSOR_FUEL_GAUGE,
	SENSOR_STORAGE,
	SENSOR_COUNT,
};

/** 2-bit values used on the wire. */
enum health_state {
	HEALTH_OK = 0,
	HEALTH_DEGRADED = 1,
	HEALTH_FAILED = 2,
	HEALTH_NOT_PRESENT = 3,
};

enum app_bus {
	BUS_A = 0, /* TWIM0: PPG + accel */
	BUS_B,     /* TWIM1: skin temp + fuel gauge */
	BUS_NONE,  /* SAADC sensors, storage */
	BUS_COUNT = BUS_NONE,
};

enum recovery_action {
	RECOVERY_NONE = 0,
	RECOVERY_BUS,         /* i2c_recover_bus() on the sensor's bus */
	RECOVERY_REINIT,      /* soft reset + reconfigure the sensor */
	RECOVERY_POWER_CYCLE, /* switch the sensor's bus rail off/on, re-init its sensors */
};

struct recovery_req {
	uint8_t sensor; /* enum sensor_id */
	uint8_t action; /* enum recovery_action */
};

/**
 * @brief Reset all state machines.
 *
 * main, before threads start. Never blocks. @return 0.
 */
int health_init(void);

/** @brief Start the health thread. main, after all inits. */
void health_start(void);

/**
 * @brief Record the outcome of a sensor's init at boot.
 *
 * -ENODEV -> NOT_PRESENT, other error -> FAILED (slow retry), 0 -> OK.
 * main / acq init path. Never blocks.
 */
void health_init_result(enum sensor_id sensor, int err);

/**
 * @brief Report one operation's result (0 = success, negative errno = failure).
 *
 * acq and storage threads. Lock-free (atomics); never blocks.
 */
void health_report(enum sensor_id sensor, int err);

/**
 * @brief Whether acq should touch this sensor now.
 *
 * False for NOT_PRESENT, and for FAILED except once per slow-retry slot.
 * acq thread. Never blocks.
 */
bool health_should_access(enum sensor_id sensor);

/**
 * @brief Take the pending recovery request, if any.
 *
 * acq thread, on EVT_RECOVER. Never blocks.
 * @return true if @p req was filled.
 */
bool health_recovery_take(struct recovery_req *req);

/** @brief Current state. Any thread; never blocks. */
enum health_state health_get_state(enum sensor_id sensor);

/** @brief All states, 2 bits per sensor. Any thread; never blocks. */
uint16_t health_get_packed(void);

/** @brief True if any sensor is FAILED. Any thread; never blocks. */
bool health_any_failed(void);

/** @brief Which bus a sensor is on. */
enum app_bus sensor_bus(enum sensor_id sensor);

/** @brief Short name ("ppg", "skin_temp", ...). NULL if out of range. */
const char *sensor_name(enum sensor_id sensor);

/** @brief Name of a health state ("OK", ...). */
const char *health_state_name(enum health_state state);

#endif /* APP_CORE_HEALTH_H_ */
