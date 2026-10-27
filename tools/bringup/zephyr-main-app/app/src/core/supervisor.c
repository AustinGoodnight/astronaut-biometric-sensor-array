/*
 * Supervisor: what main() turns into after init (ARCHITECTURE.md D4).
 * Lowest app priority; nothing it does is time-critical.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#include "app/channels.h"
#include "app/core/health.h"
#include "app/core/mode.h"
#include "app/core/power.h"
#include "app/core/supervisor.h"
#include "app/core/watchdog.h"
#include "app/record.h"
#include "app/units.h"

LOG_MODULE_REGISTER(supervisor, CONFIG_APP_LOG_LEVEL);

#define SUP_WDT_MS            30000
#define SUP_WAIT              K_SECONDS(10)
#define COUNTERS_EVERY_MS     (10 * 60 * 1000)
#define CRITICAL_BATTERY_PCT  2
#define LOW_BATTERY_EVENT_PCT 10

static K_EVENT_DEFINE(sup_events);

void supervisor_post(uint32_t events)
{
	k_event_post(&sup_events, events);
}

static void check_battery(void)
{
	static bool low_reported;
	struct metric_value m;

	if (zbus_chan_read(&chan_battery, &m, K_MSEC(5)) != 0 ||
	    !metric_is_fresh(&m, k_uptime_get_32(), BATT_MAX_AGE_MS)) {
		return;
	}
	if (m.value <= LOW_BATTERY_EVENT_PCT && !low_reported) {
		low_reported = true;
		record_emit_event(REC_EVT_LOW_BATTERY, (uint8_t)m.value, 0, 0);
	}
	if (m.value <= CRITICAL_BATTERY_PCT) {
		power_request_off(POWER_OFF_LOW_BATTERY);
	}
}

static void maybe_confirm_image(void)
{
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	/* Swap-with-revert: a new image stays a test image until it has run
	 * healthy for a while. A crash or hang before this boots the old one.
	 */
	if (!boot_is_img_confirmed() &&
	    k_uptime_get() >= (int64_t)CONFIG_APP_IMG_CONFIRM_DELAY_S * 1000 &&
	    health_get_state(SENSOR_STORAGE) == HEALTH_OK) {
		int err = boot_write_img_confirmed();

		LOG_INF("image confirmed (%d)", err);
	}
#endif
}

FUNC_NORETURN void supervisor_run(void)
{
	int wdt = app_wdt_add(SUP_WDT_MS);
	int64_t next_counters = k_uptime_get() + COUNTERS_EVERY_MS;

	for (;;) {
		uint32_t ev = k_event_wait_safe(&sup_events, SUP_EVT_MODE | SUP_EVT_POWER_OFF,
						false, SUP_WAIT);

		app_wdt_feed(wdt);

		if (ev & SUP_EVT_POWER_OFF) {
			power_off_sequence(); /* does not return */
		}
		if (ev & SUP_EVT_MODE) {
			mode_apply_pending();
		}

		check_battery();
		maybe_confirm_image();

		if (k_uptime_get() >= next_counters) {
			next_counters += COUNTERS_EVERY_MS;
			record_emit_event(REC_EVT_COUNTERS, 0, 0, 0);
		}
	}
}
