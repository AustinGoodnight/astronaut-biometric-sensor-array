/*
 * Respiration rate (publishes chan_resp, 0.01 breaths/min). The source is
 * an open decision, selected by Kconfig (APP_RESP_SRC):
 *
 *  APP_RESP_SRC_FLEX: flex sensor in a divider, sampled on the slow tick.
 *    TODO: band-pass 0.1-0.7 Hz (6-42 br/min); count peaks / zero
 *    crossings over a 30 s sliding window. (Needs a faster flex sample rate
 *    than 1 Hz for good resolution; plan ~4 Hz.)
 *
 *  APP_RESP_SRC_PPG: derived from PPG.
 *    TODO: extract three respiratory modulations per beat: amplitude
 *    (RIAV), baseline (RIIV) and beat-interval (RIFV, from
 *    hr_get_beat_intervals()); estimate the rate of each over 30-60 s and
 *    fuse them with quality weights.
 *
 * PLACEHOLDER (both): publishes 15.00 br/min at each window end if the
 * window's input was non-flat; otherwise quality 0 (-> INVALID).
 *
 * State: file-static struct resp_ctx. proc thread only.
 */

#ifndef APP_PROCESSING_RESP_H_
#define APP_PROCESSING_RESP_H_

#include <stdint.h>

#include "app/metric.h"
#include "app/ppg_block.h"

struct resp_ctx;

/** @brief main, before threads. @return 0. */
int resp_init(void);

/**
 * @brief APP_RESP_SRC_PPG input: one PPG block.
 *
 * proc thread; never blocks. A no-op returning 0 when the source is FLEX.
 */
int resp_process_block(const struct ppg_block *blk);

/**
 * @brief APP_RESP_SRC_FLEX input: one flex sample in mV.
 *
 * proc thread; never blocks. A no-op returning 0 when the source is PPG.
 */
int resp_process_flex(int32_t mv, uint32_t ts_ms);

/** @brief Bounds 4-60 br/min, rate of change, quality. Pure. proc thread. */
enum metric_status resp_validate(const struct metric_value *cand, const struct resp_ctx *ctx);

#endif /* APP_PROCESSING_RESP_H_ */
