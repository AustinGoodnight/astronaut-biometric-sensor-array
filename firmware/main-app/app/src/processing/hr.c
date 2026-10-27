/*
 * Heart rate. The algorithm is a PLACEHOLDER (see hr.h for the intended
 * one); window handling, validation and publishing are real.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/counters.h"
#include "app/processing/hr.h"
#include "app/processing/validate.h"
#include "app/units.h"

LOG_MODULE_REGISTER(hr, CONFIG_APP_LOG_LEVEL);

#define HR_IBI_HISTORY       64      /* beat intervals kept for resp / core_temp */
#define HR_MIN_AC_COUNTS     50      /* below this peak-to-peak the PPG is "flat" */
#define HR_SATURATED_COUNTS  0x3F000 /* near the 18-bit ADC ceiling */
#define HR_PLACEHOLDER       (60 * METRIC_HR_PER_BPM)

struct hr_ctx {
	/* Current record window. */
	uint32_t win_start_ms;
	bool win_has_signal;
	bool win_motion;
	/* Beat-interval ring (ms), filled by beat detection (TODO). */
	uint16_t ibi_ms[HR_IBI_HISTORY];
	uint8_t ibi_head;
	uint8_t ibi_count;
	/* Last summary that passed validation. */
	struct hr_summary last_valid;
	bool have_last_valid;
	uint32_t invalid_count;
};

static struct hr_ctx ctx;

int hr_init(void)
{
	memset(&ctx, 0, sizeof(ctx));
	return 0;
}

/* Placeholder signal check: green channel not flat and not saturated. */
static bool block_has_signal(const struct ppg_block *blk)
{
	uint32_t lo = UINT32_MAX;
	uint32_t hi = 0;

	for (int i = 0; i < blk->ppg_n; i++) {
		lo = MIN(lo, blk->ppg[i][0]);
		hi = MAX(hi, blk->ppg[i][0]);
	}
	return blk->ppg_n > 0 && (hi - lo) >= HR_MIN_AC_COUNTS && hi < HR_SATURATED_COUNTS;
}

enum metric_status hr_validate(const struct hr_summary *cand, const struct hr_ctx *c)
{
	static const struct validate_limits lim = {
		.min = 30 * METRIC_HR_PER_BPM,
		.max = 220 * METRIC_HR_PER_BPM,
		.max_rate_per_s = 10 * METRIC_HR_PER_BPM,
		.min_quality = 30,
	};

	if (cand->min > cand->avg.value || cand->max < cand->avg.value) {
		return METRIC_INVALID;
	}
	return validate_metric(&cand->avg, c->have_last_valid ? &c->last_valid.avg : NULL, &lim);
}

int hr_process_block(const struct ppg_block *blk, bool motion, struct hr_summary *out)
{
	if (ctx.win_start_ms == 0) {
		ctx.win_start_ms = blk->t_read_ms;
	}

	/* TODO: band-pass, accel-based artifact reduction, beat detection
	 * (pushes into ctx.ibi_ms), per-window avg/min/max.
	 */
	ctx.win_has_signal |= block_has_signal(blk);
	ctx.win_motion |= motion;

	if (blk->t_read_ms - ctx.win_start_ms < CONFIG_APP_RECORD_PERIOD_MS) {
		return 0;
	}

	/* Window complete: PLACEHOLDER summary. */
	struct hr_summary s = {
		.avg = {
			.value = HR_PLACEHOLDER,
			.ts_ms = blk->t_read_ms,
			.quality = ctx.win_has_signal ? (ctx.win_motion ? 50 : 90) : 0,
		},
		.min = HR_PLACEHOLDER,
		.max = HR_PLACEHOLDER,
	};

	s.avg.status = hr_validate(&s, &ctx);
	if (s.avg.status == METRIC_OK) {
		ctx.last_valid = s;
		ctx.have_last_valid = true;
	} else {
		ctx.invalid_count++;
	}

	if (zbus_chan_pub(&chan_hr, &s, K_MSEC(5)) != 0) {
		counter_inc(CNT_PUBLISH_FAIL);
	}

	ctx.win_start_ms = blk->t_read_ms;
	ctx.win_has_signal = false;
	ctx.win_motion = false;
	*out = s;
	return 1;
}

size_t hr_get_beat_intervals(uint16_t *out, size_t max)
{
	size_t n = MIN((size_t)ctx.ibi_count, max);
	size_t start = (ctx.ibi_head + HR_IBI_HISTORY - n) % HR_IBI_HISTORY;

	for (size_t i = 0; i < n; i++) {
		out[i] = ctx.ibi_ms[(start + i) % HR_IBI_HISTORY];
	}
	return n;
}
