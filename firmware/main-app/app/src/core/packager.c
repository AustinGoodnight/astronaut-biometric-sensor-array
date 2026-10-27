/*
 * Packager thread (prio 6). Deadline-driven, not data-driven: at every tick
 * it takes whatever the metric channels hold right now. A late, missing or
 * invalid metric clears its validity bit; it never delays the record.
 *
 * Why above storage/ble: a record is always built on time, even while a
 * flash erase or a BLE burst is in progress.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/core/health.h"
#include "app/core/packager.h"
#include "app/core/watchdog.h"
#include "app/counters.h"
#include "app/record.h"
#include "app/units.h"

LOG_MODULE_REGISTER(packager, CONFIG_APP_LOG_LEVEL);

#define PACKAGER_STACK_SIZE 1536
#define PACKAGER_PRIO       6
#define PACKAGER_WDT_MS     (CONFIG_APP_RECORD_PERIOD_MS * 2 + 2000)
#define CHAN_READ_TIMEOUT   K_MSEC(5)
#define MOTION_ACTIVITY_MIN (METRIC_ACTIVITY_MAX / 10)
#define LOW_BATTERY_PCT     10

static atomic_t enabled = ATOMIC_INIT(1);
static K_TIMER_DEFINE(record_timer, NULL, NULL);

void packager_set_enabled(bool on)
{
	atomic_set(&enabled, on ? 1 : 0);
}

static bool read_chan(const struct zbus_channel *chan, void *out)
{
	if (zbus_chan_read(chan, out, CHAN_READ_TIMEOUT) != 0) {
		counter_inc(CNT_ZBUS_READ_FAIL);
		return false;
	}
	return true;
}

/* Read one metric channel; true if it can go into the record. */
static bool fresh_metric(const struct zbus_channel *chan, struct metric_value *m, uint32_t now,
			 uint32_t max_age_ms)
{
	return read_chan(chan, m) && metric_is_fresh(m, now, max_age_ms);
}

static uint32_t drop_total(void)
{
	return counter_get(CNT_PPG_SLAB_FULL) + counter_get(CNT_PROC_Q_FULL) +
	       counter_get(CNT_PPG_CHIP_OVF) + counter_get(CNT_REC_DROP_STORAGE);
}

static void build_and_emit(void)
{
	static uint32_t last_drops;
	uint32_t now = k_uptime_get_32();
	struct record rec;
	struct record_data *d = &rec.data;
	struct hr_summary hr;
	struct metric_value m;

	record_begin(&rec, RECORD_TYPE_DATA);
	d->health = health_get_packed();

	if (read_chan(&chan_hr, &hr) && metric_is_fresh(&hr.avg, now, HR_MAX_AGE_MS)) {
		d->hr_avg = rec_hr_avg(hr.avg.value);
		d->hr_min = rec_hr_minmax(hr.min);
		d->hr_max = rec_hr_minmax(hr.max);
		d->validity_mask |= REC_VALID_HR_AVG | REC_VALID_HR_MIN | REC_VALID_HR_MAX;
	}
	if (fresh_metric(&chan_resp, &m, now, RESP_MAX_AGE_MS)) {
		d->resp_rate = rec_resp(m.value);
		d->validity_mask |= REC_VALID_RESP;
	}
	if (fresh_metric(&chan_skin_temp, &m, now, SKIN_TEMP_MAX_AGE_MS)) {
		d->skin_temp = rec_temp(m.value);
		d->validity_mask |= REC_VALID_SKIN_TEMP;
	}
	if (fresh_metric(&chan_core_temp, &m, now, CORE_TEMP_MAX_AGE_MS)) {
		d->core_temp = rec_temp(m.value);
		d->validity_mask |= REC_VALID_CORE_TEMP;
	}
	if (fresh_metric(&chan_gsr, &m, now, GSR_MAX_AGE_MS)) {
		d->gsr = rec_gsr(m.value);
		d->validity_mask |= REC_VALID_GSR;
	}
	if (fresh_metric(&chan_activity, &m, now, ACTIVITY_MAX_AGE_MS)) {
		d->activity = rec_activity(m.value);
		d->validity_mask |= REC_VALID_ACTIVITY;
		if (m.value >= MOTION_ACTIVITY_MIN) {
			d->flags |= REC_FLAG_MOTION;
		}
	}
	if (fresh_metric(&chan_battery, &m, now, BATT_MAX_AGE_MS)) {
		d->battery = rec_batt(m.value);
		d->validity_mask |= REC_VALID_BATTERY;
		if (d->battery <= LOW_BATTERY_PCT) {
			d->flags |= REC_FLAG_LOW_BATTERY;
		}
	}
	/* TODO: REC_FLAG_OFF_WRIST (from hr / IR), REC_FLAG_CHARGING (charger status pin). */

	if (record_time_synced()) {
		d->flags |= REC_FLAG_TIME_SYNCED;
	}

	uint32_t drops = drop_total();

	if (drops != last_drops) {
		d->flags |= REC_FLAG_DATA_DROPPED;
		last_drops = drops;
	}

	record_emit(&rec);

	LOG_INF("rec #%u valid=0x%03x health=0x%04x hr=%u.%u skin=%d gsr=%u batt=%u flags=0x%02x",
		rec.hdr.seq, d->validity_mask, d->health, d->hr_avg / 10, d->hr_avg % 10,
		d->skin_temp, d->gsr, d->battery, d->flags);
}

static void packager_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = app_wdt_add(PACKAGER_WDT_MS);

	k_timer_start(&record_timer, K_MSEC(CONFIG_APP_RECORD_PERIOD_MS),
		      K_MSEC(CONFIG_APP_RECORD_PERIOD_MS));

	for (;;) {
		/* Blocks until the next period boundary; the timer never stops. */
		(void)k_timer_status_sync(&record_timer);
		app_wdt_feed(wdt);

		if (atomic_get(&enabled)) {
			build_and_emit();
		}
	}
}

K_THREAD_DEFINE(packager_tid, PACKAGER_STACK_SIZE, packager_thread, NULL, NULL, NULL,
		PACKAGER_PRIO, 0, K_TICKS_FOREVER);

int packager_init(void)
{
	return 0;
}

void packager_start(void)
{
	k_thread_name_set(packager_tid, "packager");
	k_thread_start(packager_tid);
}
