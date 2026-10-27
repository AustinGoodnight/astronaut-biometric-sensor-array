/*
 * Packager thread (prio 6). Deadline-driven: every CONFIG_APP_RECORD_PERIOD_MS
 * it reads every metric channel, builds one DATA record (stale or missing
 * fields have their validity bit cleared) and emits it. It never waits for
 * sensors.
 */

#ifndef APP_CORE_PACKAGER_H_
#define APP_CORE_PACKAGER_H_

#include <stdbool.h>

/** @brief main, before threads. @return 0. */
int packager_init(void);

/** @brief Start the packager thread and its timer. main, after all inits. */
void packager_start(void);

/**
 * @brief Enable or disable record building (disabled during power-off).
 *
 * Any thread; never blocks.
 */
void packager_set_enabled(bool enabled);

#endif /* APP_CORE_PACKAGER_H_ */
