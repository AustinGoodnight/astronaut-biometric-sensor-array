/*
 * Per-sensor health state machine (ARCHITECTURE.md §6.2).
 *
 *            err x DEGRADE_ERRS             escalation exhausted
 *   OK ───────────────────────▶ DEGRADED ─────────────────────────▶ FAILED
 *    ▲                             │                                  │
 *    └──── ok x RECOVER_OKS ───────┘◀──── a successful op ─────────────┘
 *                                             (slow retry every FAILED_RETRY_MS)
 *
 * Why the split between threads: acq (and storage) report results with
 * atomics only, so reporting costs nothing on the hot path. All decisions
 * happen here, once per second. Recovery actions run on acq because acq
 * owns the I2C buses (D1); health hands them over through a one-slot
 * mailbox and posts EVT_RECOVER.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/comms/ble.h"
#include "app/core/acq.h"
#include "app/core/health.h"
#include "app/core/leds.h"
#include "app/core/watchdog.h"
#include "app/counters.h"
#include "app/events.h"
#include "app/record.h"
#include "app/sensors/board.h"

LOG_MODULE_REGISTER(health, CONFIG_APP_LOG_LEVEL);

#define HEALTH_TICK_MS        1000
#define HEALTH_WDT_MS         5000
#define DEGRADE_ERRS          3      /* consecutive errors: OK -> DEGRADED */
#define RECOVER_OKS           3      /* consecutive successes: DEGRADED -> OK */
#define FAILED_RETRY_MS       60000  /* slow retry while FAILED */
#define POWER_CYCLE_MIN_MS    600000 /* <= 1 power-cycle per bus per 10 min */
#define HEARTBEAT_EVERY_TICKS 5

#define HEALTH_STACK_SIZE 1536
#define HEALTH_PRIO       7

BUILD_ASSERT(SENSOR_COUNT <= 8, "health packs 2 bits per sensor into 16 bits");

/* Escalation ladder while DEGRADED; one step per tick that saw new errors. */
static const enum recovery_action ladder[] = {
	RECOVERY_NONE, /* step 0: plain retry (the next scheduled read) */
	RECOVERY_BUS,
	RECOVERY_REINIT,
	RECOVERY_POWER_CYCLE,
};

/* Written by reporters (atomics). */
struct sensor_counts {
	atomic_t err_run;
	atomic_t ok_run;
	atomic_t errs_since_tick;
};

/* Owned by the health thread. */
struct sensor_sm {
	enum health_state state;
	uint8_t step;
	int64_t next_retry_ms;
};

static struct sensor_counts counts[SENSOR_COUNT];
static struct sensor_sm sm[SENSOR_COUNT];
static int64_t last_power_cycle_ms[BUS_COUNT];

/* Read by any thread. */
static atomic_t packed;
static ATOMIC_DEFINE(retry_allowed, SENSOR_COUNT);

/* One-slot recovery mailbox: health writes, acq takes. */
static struct recovery_req mbox;
static atomic_t mbox_full;

static const char *const names[SENSOR_COUNT] = {
	[SENSOR_PPG] = "ppg",
	[SENSOR_ACCEL] = "accel",
	[SENSOR_SKIN_TEMP] = "skin_temp",
	[SENSOR_GSR] = "gsr",
	[SENSOR_FLEX] = "flex",
	[SENSOR_FUEL_GAUGE] = "fuel_gauge",
	[SENSOR_STORAGE] = "storage",
};

const char *sensor_name(enum sensor_id sensor)
{
	return sensor < SENSOR_COUNT ? names[sensor] : NULL;
}

const char *health_state_name(enum health_state state)
{
	static const char *const s[] = {"OK", "DEGRADED", "FAILED", "NOT_PRESENT"};

	return state <= HEALTH_NOT_PRESENT ? s[state] : "?";
}

enum app_bus sensor_bus(enum sensor_id sensor)
{
	switch (sensor) {
	case SENSOR_PPG:
	case SENSOR_ACCEL:
		return BUS_A;
	case SENSOR_SKIN_TEMP:
	case SENSOR_FUEL_GAUGE:
		return BUS_B;
	default:
		return BUS_NONE;
	}
}

static void set_state(enum sensor_id s, enum health_state st)
{
	sm[s].state = st;

	atomic_val_t old, new;

	do {
		old = atomic_get(&packed);
		new = (old & ~(0x3 << (2 * s))) | ((st & 0x3) << (2 * s));
	} while (!atomic_cas(&packed, old, new));
}

int health_init(void)
{
	for (int s = 0; s < SENSOR_COUNT; s++) {
		atomic_set(&counts[s].err_run, 0);
		atomic_set(&counts[s].ok_run, 0);
		atomic_set(&counts[s].errs_since_tick, 0);
		sm[s].step = 0;
		sm[s].next_retry_ms = 0;
		set_state(s, HEALTH_OK);
	}
	atomic_set(&mbox_full, 0);
	return 0;
}

void health_init_result(enum sensor_id sensor, int err)
{
	if (sensor >= SENSOR_COUNT) {
		return;
	}
	if (err == 0) {
		set_state(sensor, HEALTH_OK);
	} else if (err == -ENODEV) {
		set_state(sensor, HEALTH_NOT_PRESENT);
		LOG_INF("%s: not present", names[sensor]);
	} else {
		set_state(sensor, HEALTH_FAILED);
		sm[sensor].next_retry_ms = k_uptime_get() + FAILED_RETRY_MS;
		LOG_WRN("%s: init failed (%d), slow retry", names[sensor], err);
	}
}

void health_report(enum sensor_id sensor, int err)
{
	if (sensor >= SENSOR_COUNT) {
		return;
	}
	if (err == 0) {
		atomic_set(&counts[sensor].err_run, 0);
		atomic_inc(&counts[sensor].ok_run);
		return;
	}
	atomic_inc(&counts[sensor].err_run);
	atomic_set(&counts[sensor].ok_run, 0);
	atomic_inc(&counts[sensor].errs_since_tick);

	switch (sensor_bus(sensor)) {
	case BUS_A:
		counter_inc(CNT_I2C_ERR_A);
		break;
	case BUS_B:
		counter_inc(CNT_I2C_ERR_B);
		break;
	default:
		if (sensor == SENSOR_GSR || sensor == SENSOR_FLEX) {
			counter_inc(CNT_ANALOG_ERR);
		}
		break;
	}
}

enum health_state health_get_state(enum sensor_id sensor)
{
	if (sensor >= SENSOR_COUNT) {
		return HEALTH_NOT_PRESENT;
	}
	return (enum health_state)((atomic_get(&packed) >> (2 * sensor)) & 0x3);
}

uint16_t health_get_packed(void)
{
	return (uint16_t)atomic_get(&packed);
}

bool health_any_failed(void)
{
	for (int s = 0; s < SENSOR_COUNT; s++) {
		if (health_get_state(s) == HEALTH_FAILED) {
			return true;
		}
	}
	return false;
}

bool health_should_access(enum sensor_id sensor)
{
	switch (health_get_state(sensor)) {
	case HEALTH_OK:
	case HEALTH_DEGRADED:
		return true;
	case HEALTH_FAILED:
		return atomic_test_and_clear_bit(retry_allowed, sensor);
	default:
		return false;
	}
}

bool health_recovery_take(struct recovery_req *req)
{
	if (!atomic_get(&mbox_full)) {
		return false;
	}
	*req = mbox;
	atomic_set(&mbox_full, 0);
	return true;
}

/* @return true if the request was queued (mailbox was free). */
static bool request_recovery(enum sensor_id s, enum recovery_action action)
{
	if (atomic_get(&mbox_full)) {
		return false;
	}
	mbox.sensor = (uint8_t)s;
	mbox.action = (uint8_t)action;
	atomic_set(&mbox_full, 1);
	acq_post(EVT_RECOVER);
	return true;
}

/* Skip ladder steps that don't apply to this sensor (no bus, no rail, rate-limited). */
static bool step_applies(enum sensor_id s, enum recovery_action a, int64_t now)
{
	enum app_bus bus = sensor_bus(s);

	switch (a) {
	case RECOVERY_BUS:
		return bus != BUS_NONE;
	case RECOVERY_REINIT:
		return s != SENSOR_STORAGE;
	case RECOVERY_POWER_CYCLE:
		return bus != BUS_NONE && board_rail_present(bus) &&
		       (last_power_cycle_ms[bus] == 0 ||
			now - last_power_cycle_ms[bus] >= POWER_CYCLE_MIN_MS);
	default:
		return true;
	}
}

static void transition(enum sensor_id s, enum health_state to)
{
	enum health_state from = sm[s].state;

	if (from == to) {
		return;
	}
	set_state(s, to);
	LOG_WRN("%s: %s -> %s", names[s], health_state_name(from), health_state_name(to));

	if (to == HEALTH_FAILED || (to == HEALTH_DEGRADED && from == HEALTH_OK)) {
		record_emit_event(REC_EVT_SENSOR_FAULT, s, to, 0);
	} else {
		record_emit_event(REC_EVT_SENSOR_RECOVERED, s, to, 0);
	}
}

static void evaluate(enum sensor_id s, int64_t now)
{
	struct sensor_sm *m = &sm[s];
	atomic_val_t errs = atomic_set(&counts[s].errs_since_tick, 0);
	atomic_val_t err_run = atomic_get(&counts[s].err_run);
	atomic_val_t ok_run = atomic_get(&counts[s].ok_run);

	switch (m->state) {
	case HEALTH_OK:
		if (err_run >= DEGRADE_ERRS) {
			m->step = 0;
			transition(s, HEALTH_DEGRADED);
		}
		break;

	case HEALTH_DEGRADED:
		if (ok_run >= RECOVER_OKS) {
			transition(s, HEALTH_OK);
			break;
		}
		if (errs == 0) {
			break; /* no new errors: give the last step time to work */
		}
		/* Advance to the next applicable step. */
		while (m->step + 1U < ARRAY_SIZE(ladder)) {
			enum recovery_action a = ladder[m->step + 1];

			if (!step_applies(s, a, now)) {
				m->step++;
				continue;
			}
			if (!request_recovery(s, a)) {
				return; /* mailbox busy: try again next tick */
			}
			m->step++;
			if (a == RECOVERY_POWER_CYCLE) {
				last_power_cycle_ms[sensor_bus(s)] = now;
			}
			LOG_INF("%s: recovery step %u (action %d)", names[s], m->step, a);
			return;
		}
		/* Ladder exhausted. */
		m->next_retry_ms = now + FAILED_RETRY_MS;
		transition(s, HEALTH_FAILED);
		break;

	case HEALTH_FAILED:
		if (ok_run > 0) {
			m->step = 0;
			atomic_set(&counts[s].err_run, 0);
			transition(s, HEALTH_DEGRADED);
			break;
		}
		if (now >= m->next_retry_ms) {
			m->next_retry_ms = now + FAILED_RETRY_MS;
			if (s != SENSOR_STORAGE) {
				(void)request_recovery(s, RECOVERY_REINIT);
			}
			atomic_set_bit(retry_allowed, s);
		}
		break;

	default:
		break;
	}
}

static void update_leds(uint32_t tick)
{
	bool degraded = false;

	for (int s = 0; s < SENSOR_COUNT; s++) {
		degraded |= (sm[s].state == HEALTH_DEGRADED);
	}

	/* Red: solid while anything is FAILED, a short blink each second while DEGRADED. */
	if (health_any_failed()) {
		leds_set(LED_FAULT, true);
	} else if (degraded) {
		leds_pulse(LED_FAULT, 30);
	} else {
		leds_set(LED_FAULT, false);
	}

	if (tick % HEARTBEAT_EVERY_TICKS == 0) {
		/* Green heartbeat; blue alongside it while a central is connected. */
		leds_pulse(LED_HEARTBEAT, 20);
		if (ble_is_connected()) {
			leds_pulse(LED_LINK, 20);
		}
	}
}

static void health_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = app_wdt_add(HEALTH_WDT_MS);
	uint16_t last_packed = health_get_packed();
	uint32_t tick = 0;

	for (;;) {
		k_sleep(K_MSEC(HEALTH_TICK_MS));
		app_wdt_feed(wdt);

		int64_t now = k_uptime_get();

		for (int s = 0; s < SENSOR_COUNT; s++) {
			if (sm[s].state != HEALTH_NOT_PRESENT) {
				evaluate(s, now);
			}
		}

		uint16_t p = health_get_packed();

		if (p != last_packed) {
			struct health_msg msg = {.packed = p};

			last_packed = p;
			(void)zbus_chan_pub(&chan_health, &msg, K_MSEC(10));
		}
		update_leds(tick++);
	}
}

K_THREAD_DEFINE(health_tid, HEALTH_STACK_SIZE, health_thread, NULL, NULL, NULL, HEALTH_PRIO, 0,
		K_TICKS_FOREVER);

void health_start(void)
{
	k_thread_name_set(health_tid, "health");
	k_thread_start(health_tid);
}
