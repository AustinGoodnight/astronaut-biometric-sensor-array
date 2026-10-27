/*
 * Event bits that wake the acq thread (ARCHITECTURE.md §2.2). Posted with
 * acq_post() from ISRs, timers or other threads. ISRs post bits and nothing
 * else.
 */

#ifndef APP_EVENTS_H_
#define APP_EVENTS_H_

#include <zephyr/sys/util.h>

/** MAX30101 FIFO-almost-full interrupt. */
#define EVT_PPG_FIFO     BIT(0)
/** Accel FIFO watermark (only with CONFIG_APP_ACCEL_INT_ROUTED). */
#define EVT_ACCEL_WM     BIT(1)
/** Slow sensor timer (CONFIG_APP_SLOW_TICK_MS). */
#define EVT_SLOW_TICK    BIT(2)
/** Analog front ends have settled: do the SAADC reads. */
#define EVT_ANALOG_READY BIT(3)
/** health queued a recovery action (see health_recovery_take()). */
#define EVT_RECOVER      BIT(4)
/** Mode changed (including power-off): reconfigure sensors. */
#define EVT_MODE         BIT(5)
/** Fuel gauge ALRT pin. */
#define EVT_FG_ALERT     BIT(6)

#define EVT_ACQ_ALL (EVT_PPG_FIFO | EVT_ACCEL_WM | EVT_SLOW_TICK | EVT_ANALOG_READY | \
		     EVT_RECOVER | EVT_MODE | EVT_FG_ALERT)

#endif /* APP_EVENTS_H_ */
