/*
 * Acquisition thread (prio 2, ARCHITECTURE.md §2).
 *
 * Why one thread owns every bus: with a single owner there are no bus
 * mutexes, no priority inversion between sensor reads, and recovery
 * actions (bus recovery, re-init, power-cycle) can never race a read.
 * Why it's the highest app priority: the MAX30101 FIFO is the only hard
 * deadline in the system (~80 ms of slack after the interrupt).
 *
 * Why it never blocks on downstream: if proc stalls, acq drops data and
 * counts it; it never stops draining the chips, so the sensors and health
 * keep working and the problem shows up in the counters.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/core/acq.h"
#include "app/core/health.h"
#include "app/core/mode.h"
#include "app/core/proc.h"
#include "app/core/watchdog.h"
#include "app/counters.h"
#include "app/events.h"
#include "app/ppg_block.h"
#include "app/sensors/board.h"
#include "app/sensors/sensors.h"

LOG_MODULE_REGISTER(acq, CONFIG_APP_LOG_LEVEL);

#define ACQ_STACK_SIZE 2048
#define ACQ_PRIO       2
#define ACQ_WDT_MS     3000
/* Upper bound on the wait so the watchdog is fed even if every timer died. */
#define ACQ_MAX_WAIT   K_MSEC(1000)

#define PPG_SLAB_BLOCKS 8 /* ~2 s of slack for proc at 4 blocks/s */
#define RAIL_OFF_MS     20
#define RAIL_SETTLE_MS  5

K_MEM_SLAB_DEFINE_STATIC(ppg_slab, sizeof(struct ppg_block), PPG_SLAB_BLOCKS, 4);

/* Used when the slab is empty: the chips are still drained, the data dropped. */
static struct ppg_block scratch_block;

static K_EVENT_DEFINE(acq_events);

static bool running;      /* sensors started (false in MODE_OFF) */
static uint32_t slow_tick;
static bool analog_pending_gsr;
static bool analog_pending_flex;

static void slow_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	acq_post(EVT_SLOW_TICK);
}

static void settle_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	acq_post(EVT_ANALOG_READY);
}

static K_TIMER_DEFINE(slow_timer, slow_timer_fn, NULL);
static K_TIMER_DEFINE(settle_timer, settle_timer_fn, NULL);

void acq_post(uint32_t events)
{
	k_event_post(&acq_events, events);
}

void ppg_block_free(struct ppg_block *blk)
{
	if (blk != NULL && blk != &scratch_block) {
		k_mem_slab_free(&ppg_slab, blk);
	}
}

static void submit_sample(enum acq_msg_type type, int32_t value)
{
	struct acq_msg msg = {
		.type = (uint8_t)type,
		.sample = {.value = value, .ts_ms = k_uptime_get_32()},
	};

	if (proc_submit(&msg) != 0) {
		counter_inc(CNT_PROC_Q_FULL);
	}
}

/* --- PPG + accel pass --------------------------------------------------- */

static void ppg_pass(bool read_ppg)
{
	struct ppg_block *blk;
	bool use_ppg = read_ppg && health_should_access(SENSOR_PPG);
	bool use_accel = health_should_access(SENSOR_ACCEL);

	if (!use_ppg && !use_accel) {
		return;
	}

	if (k_mem_slab_alloc(&ppg_slab, (void **)&blk, K_NO_WAIT) != 0) {
		/* proc is behind: drain into scratch so the chip FIFOs don't overflow. */
		counter_inc(CNT_PPG_SLAB_FULL);
		blk = &scratch_block;
	}

	blk->ppg_n = 0;
	blk->accel_n = 0;
	blk->flags = 0;

	if (use_ppg) {
		(void)ppg_read(blk); /* errors already reported to health */
	}
	/* Same pass: drain the accel so both blocks cover the same time span. */
	if (use_accel) {
		(void)accel_read(blk);
	}
	blk->t_read_ms = k_uptime_get_32();

	if (blk == &scratch_block || (blk->ppg_n == 0 && blk->accel_n == 0)) {
		ppg_block_free(blk);
		return;
	}

	struct acq_msg msg = {.type = ACQ_MSG_PPG_BLOCK, .blk = blk};

	if (proc_submit(&msg) != 0) {
		counter_inc(CNT_PROC_Q_FULL);
		ppg_block_free(blk);
	}
}

/* --- slow sensors ------------------------------------------------------- */

/* slow_tick starts at 0, so every slow sensor is read on the first tick after boot. */
static bool due(uint32_t div)
{
	return div != 0 && (slow_tick % div) == 0;
}

static void slow_pass(void)
{
	if (due(CONFIG_APP_SKIN_TEMP_DIV) && health_should_access(SENSOR_SKIN_TEMP)) {
		int32_t mdegc;

		if (skin_temp_read(&mdegc) == 0) {
			submit_sample(ACQ_MSG_SKIN_TEMP, mdegc);
		}
	}

	if (due(CONFIG_APP_BATTERY_DIV) && health_should_access(SENSOR_FUEL_GAUGE)) {
		uint8_t soc;

		if (fuel_gauge_read(&soc) == 0) {
			submit_sample(ACQ_MSG_BATTERY, soc);
		}
	}

	/* No PPG interrupts (PPG down or off): keep the accel drained here. */
	if (health_get_state(SENSOR_PPG) != HEALTH_OK && !IS_ENABLED(CONFIG_APP_ACCEL_INT_ROUTED)) {
		ppg_pass(false);
	}

	/* Analog phase 1: power the front ends and wait for them to settle
	 * (a one-shot timer, not a sleep: acq stays responsive to the PPG).
	 */
	analog_pending_gsr = due(CONFIG_APP_GSR_DIV) && health_should_access(SENSOR_GSR);
	analog_pending_flex = IS_ENABLED(CONFIG_APP_RESP_SRC_FLEX) && due(CONFIG_APP_FLEX_DIV) &&
			      health_should_access(SENSOR_FLEX);

	if (analog_pending_gsr) {
		gsr_power(true);
	}
	if (analog_pending_flex) {
		flex_power(true);
	}
	if (analog_pending_gsr || analog_pending_flex) {
		k_timer_start(&settle_timer, K_MSEC(CONFIG_APP_ANALOG_SETTLE_MS), K_NO_WAIT);
	}
	slow_tick++;
}

static void analog_pass(void)
{
	int32_t mv;

	/* Phase 2: read, then power the front ends down. */
	if (analog_pending_gsr) {
		if (gsr_read(&mv) == 0) {
			submit_sample(ACQ_MSG_GSR, mv);
		}
		gsr_power(false);
	}
	if (analog_pending_flex) {
		if (flex_read(&mv) == 0) {
			submit_sample(ACQ_MSG_FLEX, mv);
		}
		flex_power(false);
	}
	analog_pending_gsr = false;
	analog_pending_flex = false;
}

/* --- start / stop / recovery --------------------------------------------- */

static bool usable(enum sensor_id s)
{
	enum health_state st = health_get_state(s);

	return st == HEALTH_OK || st == HEALTH_DEGRADED;
}

static void sensors_start(void)
{
	/* FAILED sensors are started by their slow-retry re-init instead. */
	if (usable(SENSOR_PPG)) {
		(void)ppg_start();
	}
	if (usable(SENSOR_ACCEL)) {
		(void)accel_start();
	}
	running = true;
}

static void sensors_stop(void)
{
	/* Interrupts off before power off (sensors.h power-sequencing rule). */
	(void)ppg_stop();
	(void)accel_stop();
	gsr_power(false);
	flex_power(false);
	analog_pending_gsr = false;
	analog_pending_flex = false;
	(void)board_rail_set(BUS_A, false);
	(void)board_rail_set(BUS_B, false);
	running = false;
}

static void power_cycle_bus(enum app_bus bus)
{
	counter_inc(CNT_POWER_CYCLES);
	LOG_WRN("power-cycling bus %c", bus == BUS_A ? 'A' : 'B');

	if (bus == BUS_A) {
		(void)ppg_stop();
		(void)accel_stop();
	}
	(void)board_rail_set(bus, false);
	k_msleep(RAIL_OFF_MS); /* bounded; the PPG FIFO has ~300 ms of headroom */
	(void)board_rail_set(bus, true);
	k_msleep(RAIL_SETTLE_MS);

	for (int s = 0; s < SENSOR_COUNT; s++) {
		if (sensor_bus(s) == bus && health_get_state(s) != HEALTH_NOT_PRESENT) {
			(void)sensors_reinit(s);
		}
	}
}

static void run_recovery(void)
{
	struct recovery_req req;

	if (!health_recovery_take(&req)) {
		return;
	}

	enum sensor_id s = (enum sensor_id)req.sensor;
	int err = 0;

	switch (req.action) {
	case RECOVERY_BUS:
		counter_inc(CNT_BUS_RECOVERIES);
		err = board_bus_recover(sensor_bus(s));
		LOG_INF("%s: bus recovery -> %d", sensor_name(s), err);
		break;
	case RECOVERY_REINIT:
		err = sensors_reinit(s);
		LOG_INF("%s: re-init -> %d", sensor_name(s), err);
		break;
	case RECOVERY_POWER_CYCLE:
		power_cycle_bus(sensor_bus(s));
		break;
	default:
		break;
	}
}

static void handle_mode(void)
{
	enum app_mode m = mode_get();

	if (m == MODE_OFF) {
		if (running) {
			LOG_INF("stopping sensors");
			sensors_stop();
		}
		k_timer_stop(&slow_timer);
	} else if (!running) {
		(void)board_rail_set(BUS_A, true);
		(void)board_rail_set(BUS_B, true);
		sensors_start();
	}
	/* TODO: MODE_RAW_WAVEFORM (stretch): also hand PPG blocks to storage. */
}

/* --- thread ---------------------------------------------------------------- */

static void acq_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = app_wdt_add(ACQ_WDT_MS);

	sensors_start();
	k_timer_start(&slow_timer, K_MSEC(CONFIG_APP_SLOW_TICK_MS),
		      K_MSEC(CONFIG_APP_SLOW_TICK_MS));

	for (;;) {
		/* _safe: atomically clears exactly the bits it returns. */
		uint32_t ev = k_event_wait_safe(&acq_events, EVT_ACQ_ALL, false, ACQ_MAX_WAIT);

		app_wdt_feed(wdt);

		if (ev & EVT_MODE) {
			handle_mode();
		}
		if (!running) {
			continue;
		}
		if (ev & EVT_RECOVER) {
			run_recovery();
		}
		if (ev & EVT_PPG_FIFO) {
			ppg_pass(true);
		} else if (ev & EVT_ACCEL_WM) {
			ppg_pass(false);
		}
		if (ev & EVT_ANALOG_READY) {
			analog_pass();
		}
		if (ev & EVT_SLOW_TICK) {
			slow_pass();
		}
		if (ev & EVT_FG_ALERT) {
			/* TODO: read SOC now and clear the alert (fg_drv_clear_alert()). */
			LOG_WRN("fuel gauge alert");
		}
	}
}

K_THREAD_DEFINE(acq_tid, ACQ_STACK_SIZE, acq_thread, NULL, NULL, NULL, ACQ_PRIO, 0,
		K_TICKS_FOREVER);

int acq_init(void)
{
	int err = board_init();

	if (err) {
		LOG_ERR("board_init: %d", err);
	}

	/* A failure here marks the sensor (FAILED / NOT_PRESENT); it is never fatal. */
	health_init_result(SENSOR_PPG, ppg_init());
	health_init_result(SENSOR_ACCEL, accel_init());
	health_init_result(SENSOR_SKIN_TEMP, skin_temp_init());
	health_init_result(SENSOR_GSR, gsr_init());
	health_init_result(SENSOR_FLEX, IS_ENABLED(CONFIG_APP_RESP_SRC_FLEX) ? flex_init()
									  : -ENODEV);
	health_init_result(SENSOR_FUEL_GAUGE, fuel_gauge_init());
	return 0;
}

void acq_start(void)
{
	k_thread_name_set(acq_tid, "acq");
	k_thread_start(acq_tid);
}
