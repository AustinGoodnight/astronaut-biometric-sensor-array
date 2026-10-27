/*
 * Estimated core temperature from HR. The Kalman filter is a PLACEHOLDER
 * (see core_temp.h for the ECTemp-style model to implement).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/counters.h"
#include "app/processing/core_temp.h"
#include "app/processing/validate.h"
#include "app/units.h"

#define CORE_TEMP_PLACEHOLDER (37 * METRIC_TEMP_PER_DEGC)

struct core_temp_ctx {
	/* Kalman state (TODO): estimate (°C) and its variance. Long-lived: reset only at boot. */
	float ct;
	float var;
	uint32_t updates;
	struct metric_value last_valid;
	bool have_last_valid;
};

static struct core_temp_ctx ctx;

int core_temp_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	ctx.ct = 37.1f; /* ECTemp initial value */
	ctx.var = 0.0f;
	return 0;
}

enum metric_status core_temp_validate(const struct metric_value *cand,
				      const struct core_temp_ctx *c)
{
	static const struct validate_limits lim = {
		.min = 35 * METRIC_TEMP_PER_DEGC,
		.max = 41 * METRIC_TEMP_PER_DEGC,
		.max_rate_per_s = METRIC_TEMP_PER_DEGC / 60, /* 1 °C per minute */
		.min_quality = 30,
	};

	return validate_metric(cand, c->have_last_valid ? &c->last_valid : NULL, &lim);
}

int core_temp_update(const struct hr_summary *hr)
{
	bool hr_ok = hr->avg.status == METRIC_OK;

	/* TODO: predict (var += gamma^2); if hr_ok, update with the HR observation. */
	ctx.updates++;

	struct metric_value m = {
		.value = CORE_TEMP_PLACEHOLDER,
		.ts_ms = hr->avg.ts_ms,
		.quality = hr_ok ? 50 : 0,
	};

	m.status = core_temp_validate(&m, &ctx);
	if (m.status == METRIC_OK) {
		ctx.last_valid = m;
		ctx.have_last_valid = true;
	}
	if (zbus_chan_pub(&chan_core_temp, &m, K_MSEC(5)) != 0) {
		counter_inc(CNT_PUBLISH_FAIL);
	}
	return 0;
}
