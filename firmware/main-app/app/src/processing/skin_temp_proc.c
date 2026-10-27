/*
 * Skin temperature. Smoothing is a PLACEHOLDER (pass-through); validation is real.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/counters.h"
#include "app/processing/skin_temp_proc.h"
#include "app/processing/validate.h"
#include "app/units.h"

struct skin_temp_ctx {
	/* TODO: last 3 raw samples (median), EMA state. */
	int32_t hist[3];
	uint8_t hist_n;
	struct metric_value last_valid;
	bool have_last_valid;
};

static struct skin_temp_ctx ctx;

int skin_temp_proc_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	return 0;
}

enum metric_status skin_temp_validate(const struct metric_value *cand,
				      const struct skin_temp_ctx *c)
{
	static const struct validate_limits lim = {
		.min = 20 * METRIC_TEMP_PER_DEGC,
		.max = 42 * METRIC_TEMP_PER_DEGC,
		.max_rate_per_s = METRIC_TEMP_PER_DEGC / 2, /* 0.5 °C/s */
		.min_quality = 50,
	};

	return validate_metric(cand, c->have_last_valid ? &c->last_valid : NULL, &lim);
}

int skin_temp_proc_process(int32_t mdegc, uint32_t ts_ms)
{
	struct metric_value m = {.value = mdegc, .ts_ms = ts_ms, .quality = 100};

	m.status = skin_temp_validate(&m, &ctx);
	if (m.status == METRIC_OK) {
		ctx.last_valid = m;
		ctx.have_last_valid = true;
	}
	if (zbus_chan_pub(&chan_skin_temp, &m, K_MSEC(5)) != 0) {
		counter_inc(CNT_PUBLISH_FAIL);
	}
	return 0;
}
