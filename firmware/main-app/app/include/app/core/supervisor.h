/*
 * Supervisor: the main thread after init (prio 10). Applies mode changes,
 * runs the power-off sequence, emits the periodic COUNTERS event, watches
 * for critical battery, and confirms a new MCUboot image once healthy.
 */

#ifndef APP_CORE_SUPERVISOR_H_
#define APP_CORE_SUPERVISOR_H_

#include <stdint.h>
#include <zephyr/sys/util.h>

#define SUP_EVT_MODE      BIT(0)
#define SUP_EVT_POWER_OFF BIT(1)

/** @brief Wake the supervisor. Any context; never blocks. */
void supervisor_post(uint32_t events);

/** @brief Supervisor loop. main only, after starting threads. Does not return. */
FUNC_NORETURN void supervisor_run(void);

#endif /* APP_CORE_SUPERVISOR_H_ */
