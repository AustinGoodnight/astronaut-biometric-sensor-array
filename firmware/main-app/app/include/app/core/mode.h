/*
 * Operating mode (ARCHITECTURE.md §10.1). Persisted in settings. Requests
 * come from the button (stub), BLE SET_MODE or the shell. The supervisor
 * applies them in order: flush storage, MODE_CHANGE event, publish chan_mode.
 */

#ifndef APP_CORE_MODE_H_
#define APP_CORE_MODE_H_

#include <stdint.h>

enum app_mode {
	MODE_NORMAL = 0,       /* BLE + log */
	MODE_LOG_ONLY = 1,     /* BLE off */
	MODE_RAW_WAVEFORM = 2, /* stretch goal (stub): also log PPG waveform */
	MODE_OFF = 3,          /* power-off in progress; never persisted */
	MODE_COUNT,
};

enum mode_source {
	MODE_SRC_BOOT = 0,
	MODE_SRC_BUTTON,
	MODE_SRC_BLE,
	MODE_SRC_SHELL,
	MODE_SRC_POWER,
};

/** @brief Load the persisted mode. main, after persist_init(). @return 0. */
int mode_init(void);

/** @brief Current mode. Any thread; never blocks. */
enum app_mode mode_get(void);

/**
 * @brief Ask for a mode change; the supervisor applies it.
 *
 * Any thread (including BT and input callbacks). Never blocks.
 * @return 0, or -EINVAL for a mode that can't be requested.
 */
int mode_request(enum app_mode mode, enum mode_source source);

/**
 * @brief Apply a pending request (flush, persist, event record, publish).
 *
 * Supervisor thread only. Blocks for up to the storage flush timeout.
 */
void mode_apply_pending(void);

/**
 * @brief Switch to MODE_OFF immediately (power-off sequence). Not persisted.
 *
 * Supervisor thread only. Never blocks.
 */
void mode_enter_off(void);

/** @brief Name for logs/shell. */
const char *mode_name(enum app_mode mode);

#endif /* APP_CORE_MODE_H_ */
