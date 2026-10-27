/*
 * Per-metric result published on zbus by the processing modules and read by
 * the packager. Scales are in units.h.
 */

#ifndef APP_METRIC_H_
#define APP_METRIC_H_

#include <stdbool.h>
#include <stdint.h>

enum metric_status {
	METRIC_OK = 0,
	/** Set by the packager: ts older than the metric's max age. */
	METRIC_STALE,
	/** Set by *_validate(): out of bounds, too fast a change, or low quality. */
	METRIC_INVALID,
	/** The source sensor is FAILED. */
	METRIC_SENSOR_FAULT,
	/** Compiled out or sensor absent. */
	METRIC_NOT_PRESENT,
};

struct metric_value {
	/** Fixed point, scale per metric in units.h. */
	int32_t value;
	/** Uptime (ms) of the newest input sample that produced this value. 0 = never. */
	uint32_t ts_ms;
	/** Signal quality 0..100. */
	uint8_t quality;
	/** enum metric_status. */
	uint8_t status;
};

/** HR over one record window: avg plus extremes (same scale as avg). */
struct hr_summary {
	struct metric_value avg;
	int32_t min;
	int32_t max;
};

/**
 * @brief True if a value can go into a record: status OK and not older than
 *        @p max_age_ms at @p now_ms.
 *
 * Any thread; never blocks.
 */
static inline bool metric_is_fresh(const struct metric_value *m, uint32_t now_ms,
				   uint32_t max_age_ms)
{
	return m->status == METRIC_OK && m->ts_ms != 0 && (now_ms - m->ts_ms) <= max_age_ms;
}

#endif /* APP_METRIC_H_ */
