/*
 * Task watchdog wrapper (ARCHITECTURE.md §6.1). Each thread registers its
 * own channel and feeds it every loop. Backed by the nRF hardware WDT. If a
 * channel starves, the board reboots and the next BOOT event names it.
 */

#ifndef APP_CORE_WATCHDOG_H_
#define APP_CORE_WATCHDOG_H_

#include <stdint.h>

/** @brief Init task_wdt with the hardware fallback. main, first. @return 0 or -errno. */
int app_wdt_init(void);

/**
 * @brief Register a channel for the calling thread.
 *
 * Call at the top of the thread entry. Never blocks.
 * @return channel id (>= 0), or negative errno (feeding a negative id is a no-op).
 */
int app_wdt_add(uint32_t timeout_ms);

/** @brief Feed a channel. Owning thread; never blocks. */
void app_wdt_feed(int channel);

/**
 * @brief Channel that starved before the last reset, if any.
 *
 * main, once at boot (clears it). @return channel id, or -1 if the last reset
 *         was not a task-watchdog timeout.
 */
int app_wdt_last_starved(void);

#endif /* APP_CORE_WATCHDOG_H_ */
