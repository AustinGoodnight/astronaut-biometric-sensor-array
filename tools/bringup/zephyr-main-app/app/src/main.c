/*
 * Wrist module firmware: init in a fixed order, start the threads, then
 * become the supervisor (ARCHITECTURE.md §12).
 *
 * Nothing in init is fatal. A sensor, settings or storage failure is
 * logged and reflected in health/records, and the board keeps running:
 * a degraded wrist module that still reports is worth more than a
 * reboot loop.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/comms/ble.h"
#include "app/core/acq.h"
#include "app/core/health.h"
#include "app/core/leds.h"
#include "app/core/mode.h"
#include "app/core/packager.h"
#include "app/core/persist.h"
#include "app/core/power.h"
#include "app/core/proc.h"
#include "app/core/supervisor.h"
#include "app/core/watchdog.h"
#include "app/record.h"
#include "app/storage/storage.h"

LOG_MODULE_REGISTER(main, CONFIG_APP_LOG_LEVEL);

#define CHECK(expr)                                                                                \
	do {                                                                                       \
		int _err = (expr);                                                                 \
		if (_err) {                                                                        \
			LOG_ERR("%s failed: %d", #expr, _err);                                     \
		}                                                                                  \
	} while (0)

int main(void)
{
	/* 1-2. Boot count and reset cause, before any record is built. */
	CHECK(persist_init());
	int starved = app_wdt_last_starved();

	/* 3. Record numbering for this boot. */
	record_init(persist_boot_count());

	/* 4. Watchdog first among the runtime pieces, so it covers everything after. */
	CHECK(app_wdt_init());

	/* 5. Health (sensors register their init results with it) and LEDs. */
	CHECK(health_init());
	CHECK(leds_init());

	/* 6. Storage (mount / find the head) and mode. */
	(void)storage_init(); /* failure is recorded as storage health */
	CHECK(mode_init());

	/* 7. Board power + every sensor. Failures mark sensors, never fatal. */
	CHECK(acq_init());

	/* 8. Processing modules and the packager. */
	CHECK(proc_init());
	CHECK(packager_init());

	/* 9. Button. */
	CHECK(power_init());

	/* 10. BLE stack. */
	CHECK(ble_init());

	/* 11. First record of the boot. */
	record_emit_event(REC_EVT_BOOT, 0, (uint16_t)(starved + 1), persist_reset_cause());
	if (starved >= 0) {
		LOG_WRN("last reset: task watchdog, channel %d starved", starved);
	}

	/* 12. Consumers before producers, so nothing is produced into a void. */
	storage_start();
	ble_start();
	proc_start();
	packager_start();
	health_start();
	acq_start();

	LOG_INF("wrist module running (boot #%u, mode %s)", persist_boot_count(),
		mode_name(mode_get()));

	/* 13. main becomes the supervisor thread. */
	supervisor_run();
	return 0;
}
