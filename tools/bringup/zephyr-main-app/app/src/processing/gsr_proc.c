/*
 * GSR. Conversion and smoothing are PLACEHOLDERS (see gsr_proc.h); validation is real.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/counters.h"
#include "app/processing/gsr_proc.h"
#include "app/processing/validate.h"
#include "app/units.h"

#define GSR_PLACEHOLDER_NS_PER_MV 10 /* NOT physical; replace with the front-end transfer function */

struct gsr_ctx {
	/* TODO: EMA state (tonic level), phasic peak tracking, electrode-off detector. */
	int32_t ema;
	struct metric_value last_valid;
	bool have_last_valid;
};

static struct gsr_ctx ctx;

int gsr_proc_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	return 0;
}

enum metric_status gsr_validate(const struct metric_value *cand, const struct gsr_ctx *c)
{
	static const struct validate_limits lim = {
		.min = 0,
		.max = 100 * METRIC_GSR_PER_US,
		.max_rate_per_s = 0,
		.min_quality = 30,
	};

	ARG_UNUSED(c);
	return validate_metric(cand, NULL, &lim);
}

int gsr_proc_process(int32_t mv, uint32_t ts_ms)
{
	struct metric_value m = {
		.value = mv * GSR_PLACEHOLDER_NS_PER_MV,
		.ts_ms = ts_ms,
		.quality = 100,
	};

	m.status = gsr_validate(&m, &ctx);
	if (m.status == METRIC_OK) {
		ctx.last_valid = m;
		ctx.have_last_valid = true;
	}
	if (zbus_chan_pub(&chan_gsr, &m, K_MSEC(5)) != 0) {
		counter_inc(CNT_PUBLISH_FAIL);
	}
	return 0;
}
