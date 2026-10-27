#include <stdlib.h>
#include <zephyr/sys/util.h>

#include "app/processing/validate.h"

enum metric_status validate_metric(const struct metric_value *cand,
				   const struct metric_value *last_valid,
				   const struct validate_limits *lim)
{
	if (cand->value < lim->min || cand->value > lim->max) {
		return METRIC_INVALID;
	}
	if (cand->quality < lim->min_quality) {
		return METRIC_INVALID;
	}

	if (lim->max_rate_per_s > 0 && last_valid != NULL && last_valid->ts_ms != 0) {
		uint32_t dt_ms = cand->ts_ms - last_valid->ts_ms;

		/* Only compare against a recent value; after a long gap anything goes. */
		if (dt_ms <= VALIDATE_RATE_WINDOW_MS) {
			/* Allow at least one second's worth of change. */
			int64_t allowed = (int64_t)lim->max_rate_per_s * MAX(dt_ms, 1000U) / 1000;
			int64_t diff = llabs((int64_t)cand->value - last_valid->value);

			if (diff > allowed) {
				return METRIC_INVALID;
			}
		}
	}
	return METRIC_OK;
}
