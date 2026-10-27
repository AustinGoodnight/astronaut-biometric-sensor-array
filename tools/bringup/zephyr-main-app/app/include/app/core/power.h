/*
 * Power button and power-off sequence (ARCHITECTURE.md §10.2).
 */

#ifndef APP_CORE_POWER_H_
#define APP_CORE_POWER_H_

enum power_off_reason {
	POWER_OFF_BUTTON = 1,
	POWER_OFF_LOW_BATTERY,
	POWER_OFF_SHELL,
};

/** @brief Set up the button (input subsystem long-press). main, before threads. @return 0. */
int power_init(void);

/**
 * @brief Ask the supervisor to power off.
 *
 * Any thread, including input/BT callbacks. Never blocks.
 */
void power_request_off(enum power_off_reason reason);

/**
 * @brief Run the power-off sequence: stop sensors, POWER_OFF + COUNTERS
 *        events, flush storage, release power-hold, then System OFF.
 *
 * Supervisor thread only. Blocks for up to ~3 s, then does not return.
 */
void power_off_sequence(void);

#endif /* APP_CORE_POWER_H_ */
