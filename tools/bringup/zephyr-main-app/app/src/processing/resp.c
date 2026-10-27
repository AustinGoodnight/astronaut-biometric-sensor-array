/*
 * Respiration rate. Source selected by Kconfig (APP_RESP_SRC_PPG /
 * APP_RESP_SRC_FLEX). Both algorithms are PLACEHOLDERS (see resp.h).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/counters.h"
#include "app/processing/resp.h"
#include "app/processing/validate.h"
#include "app/units.h"

#define RESP_PLACEHOLDER    (15 * METRIC_RESP_PER_BRPM)
#define RESP_FLEX_MIN_AC_MV 5
#define RESP_PPG_MIN_AC     50

struct resp_ctx {
	uint32_t win_start_ms;
	int32_t win_lo;
	int32_t win_hi;
	/* TODO: band-pass state, peak history (FLEX); RIAV/RIIV/RIFV series (PPG). */
	struct metric_value last_valid;
	bool have_last_valid;
};

static struct resp_ctx ctx;

static void window_reset(uint32_t now)
{
	ctx.win_start_ms = now;
	ctx.win_lo = INT32_MAX;
	ctx.win_hi = INT32_MIN;
}

int resp_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	window_reset(0);
	return 0;
}

enum metric_status resp_validate(const struct metric_value *cand, const struct resp_ctx *c)
{
	static const struct validate_limits lim = {
		.min = 4 * METRIC_RESP_PER_BRPM,
		.max = 60 * METRIC_RESP_PER_BRPM,
		.max_rate_per_s = 2 * METRIC_RESP_PER_BRPM,
		.min_quality = 30,
	};

	return validate_metric(cand, c->have_last_valid ? &c->last_valid : NULL, &lim);
}

/* Feed one input value; publish a PLACEHOLDER rate at each window end. */
static void window_feed(int32_t v, uint32_t ts_ms, int32_t min_ac)
{
	if (ctx.win_start_ms == 0) {
		window_reset(ts_ms);
	}
	ctx.win_lo = MIN(ctx.win_lo, v);
	ctx.win_hi = MAX(ctx.win_hi, v);

	if (ts_ms - ctx.win_start_ms < CONFIG_APP_RECORD_PERIOD_MS) {
		return;
	}

	bool varying = (int64_t)ctx.win_hi - ctx.win_lo >= min_ac;
	struct metric_value m = {
		.value = RESP_PLACEHOLDER,
		.ts_ms = ts_ms,
		.quality = varying ? 80 : 0,
	};

	m.status = resp_validate(&m, &ctx);
	if (m.status == METRIC_OK) {
		ctx.last_valid = m;
		ctx.have_last_valid = true;
	}
	if (zbus_chan_pub(&chan_resp, &m, K_MSEC(5)) != 0) {
		counter_inc(CNT_PUBLISH_FAIL);
	}
	window_reset(ts_ms);
}

int resp_process_block(const struct ppg_block *blk)
{
	if (!IS_ENABLED(CONFIG_APP_RESP_SRC_PPG)) {
		return 0;
	}
	/* TODO: per-beat RIAV / RIIV / RIFV extraction. Placeholder: track the
	 * block's green-channel range.
	 */
	for (int i = 0; i < blk->ppg_n; i++) {
		window_feed((int32_t)blk->ppg[i][0], blk->t_read_ms, RESP_PPG_MIN_AC);
	}
	return 0;
}

int resp_process_flex(int32_t mv, uint32_t ts_ms)
{
	if (!IS_ENABLED(CONFIG_APP_RESP_SRC_FLEX)) {
		return 0;
	}
	/* TODO: band-pass 0.1-0.7 Hz and peak counting over 30 s. */
	window_feed(mv, ts_ms, RESP_FLEX_MIN_AC_MV);
	return 0;
}
