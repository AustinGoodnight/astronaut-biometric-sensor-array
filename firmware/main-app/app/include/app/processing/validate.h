/*
 * Shared validation helper used by every module's *_validate()
 * (ARCHITECTURE.md §3.4): physiological bounds, maximum rate of change
 * versus the last VALID value, minimum quality. Invalid results are
 * flagged, never replaced.
 */

#ifndef APP_PROCESSING_VALIDATE_H_
#define APP_PROCESSING_VALIDATE_H_

#include <stdint.h>

#include "app/metric.h"

/** A rate check only applies if the last valid value is at most this old. */
#define VALIDATE_RATE_WINDOW_MS 60000

struct validate_limits {
	int32_t min;
	int32_t max;
	/** Max |change| per second versus the last valid value; 0 = no check. */
	int32_t max_rate_per_s;
	/** Minimum quality (0..100). */
	uint8_t min_quality;
};

/**
 * @brief Check a candidate value.
 *
 * Pure function. proc thread; never blocks.
 *
 * @param cand       Candidate (value, ts_ms, quality used).
 * @param last_valid Last value that passed, or NULL if none.
 * @return METRIC_OK or METRIC_INVALID.
 */
enum metric_status validate_metric(const struct metric_value *cand,
				   const struct metric_value *last_valid,
				   const struct validate_limits *lim);

#endif /* APP_PROCESSING_VALIDATE_H_ */
