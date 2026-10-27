/*
 * zbus channels (ARCHITECTURE.md §3.5). Defined in src/core/channels.c.
 *
 * Metric channels hold the LATEST value (not a queue). Publishers:
 * processing modules on the proc thread. Reader: the packager, with a
 * short timeout.
 */

#ifndef APP_CHANNELS_H_
#define APP_CHANNELS_H_

#include <stdint.h>
#include <zephyr/zbus/zbus.h>

#include "app/metric.h"

/** Published by health when any sensor's state changes. */
struct health_msg {
	/** 2 bits per sensor, same packing as record_data.health. */
	uint16_t packed;
};

/** Published by the supervisor when the mode changes (including MODE_OFF). */
struct mode_msg {
	uint8_t mode;   /* enum app_mode */
	uint8_t source; /* enum mode_source */
};

/** Published by ble when the helmet sends HELMET_STATUS. */
struct link_msg {
	uint8_t lora_link;
	uint8_t helmet_batt_pct;
	uint8_t flags;
	uint32_t ts_ms;
};

/* struct hr_summary */
ZBUS_CHAN_DECLARE(chan_hr);
/* struct metric_value */
ZBUS_CHAN_DECLARE(chan_resp, chan_skin_temp, chan_core_temp, chan_gsr, chan_activity,
		  chan_battery);
/* system channels */
ZBUS_CHAN_DECLARE(chan_health, chan_mode, chan_link);

#endif /* APP_CHANNELS_H_ */
