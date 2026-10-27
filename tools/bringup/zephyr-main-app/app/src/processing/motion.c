/*
 * Activity / motion. The algorithm is a PLACEHOLDER (see motion.h).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/counters.h"
#include "app/processing/motion.h"
#include "app/processing/validate.h"
#include "app/units.h"

struct motion_ctx {
	uint32_t win_start_ms;
	uint32_t win_samples;
	/* TODO: high-pass filter state, activity accumulator. */
	struct metric_value last_valid;
	bool have_last_valid;
};

static struct motion_ctx ctx;

int motion_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	return 0;
}

enum metric_status motion_validate(const struct metric_value *cand, const struct motion_ctx *c)
{
	static const struct validate_limits lim = {
		.min = 0, .max = METRIC_ACTIVITY_MAX, .max_rate_per_s = 0, .min_quality = 10,
	};

	ARG_UNUSED(c);
	return validate_metric(cand, NULL, &lim);
}

int motion_process_block(const struct ppg_block *blk, bool *motion)
{
	*motion = false; /* TODO: short-term energy threshold */

	if (ctx.win_start_ms == 0) {
		ctx.win_start_ms = blk->t_read_ms;
	}
	ctx.win_samples += blk->accel_n;

	if (blk->t_read_ms - ctx.win_start_ms < CONFIG_APP_RECORD_PERIOD_MS) {
		return 0;
	}

	if (ctx.win_samples > 0) {
		/* PLACEHOLDER activity value. */
		struct metric_value m = {.value = 0, .ts_ms = blk->t_read_ms, .quality = 100};

		m.status = motion_validate(&m, &ctx);
		if (m.status == METRIC_OK) {
			ctx.last_valid = m;
			ctx.have_last_valid = true;
		}
		if (zbus_chan_pub(&chan_activity, &m, K_MSEC(5)) != 0) {
			counter_inc(CNT_PUBLISH_FAIL);
		}
	}

	ctx.win_start_ms = blk->t_read_ms;
	ctx.win_samples = 0;
	return 0;
}
