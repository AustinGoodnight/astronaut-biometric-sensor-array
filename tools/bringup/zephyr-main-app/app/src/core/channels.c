/*
 * zbus channel definitions. Observers are attached where they live
 * (ZBUS_CHAN_ADD_OBS in the observing module), so this file has no
 * dependencies on the modules.
 *
 * Metric channels start as STALE with ts 0, so the packager treats them as
 * missing until a processing module publishes.
 */

#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/core/health.h"
#include "app/core/mode.h"

#define METRIC_INIT ZBUS_MSG_INIT(.value = 0, .ts_ms = 0, .quality = 0, .status = METRIC_STALE)

ZBUS_CHAN_DEFINE(chan_hr, struct hr_summary, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.avg = {.status = METRIC_STALE}, .min = 0, .max = 0));

ZBUS_CHAN_DEFINE(chan_resp, struct metric_value, NULL, NULL, ZBUS_OBSERVERS_EMPTY, METRIC_INIT);
ZBUS_CHAN_DEFINE(chan_skin_temp, struct metric_value, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 METRIC_INIT);
ZBUS_CHAN_DEFINE(chan_core_temp, struct metric_value, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 METRIC_INIT);
ZBUS_CHAN_DEFINE(chan_gsr, struct metric_value, NULL, NULL, ZBUS_OBSERVERS_EMPTY, METRIC_INIT);
ZBUS_CHAN_DEFINE(chan_activity, struct metric_value, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 METRIC_INIT);
ZBUS_CHAN_DEFINE(chan_battery, struct metric_value, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 METRIC_INIT);

ZBUS_CHAN_DEFINE(chan_health, struct health_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.packed = 0));
ZBUS_CHAN_DEFINE(chan_mode, struct mode_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.mode = MODE_NORMAL, .source = MODE_SRC_BOOT));
ZBUS_CHAN_DEFINE(chan_link, struct link_msg, NULL, NULL, ZBUS_OBSERVERS_EMPTY,
		 ZBUS_MSG_INIT(.lora_link = 0, .helmet_batt_pct = 0, .flags = 0, .ts_ms = 0));
