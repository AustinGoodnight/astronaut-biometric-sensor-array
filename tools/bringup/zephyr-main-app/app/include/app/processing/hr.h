/*
 * Heart rate from PPG (publishes chan_hr, struct hr_summary).
 *
 * Intended algorithm (TODO; the current code is a placeholder, ARCHITECTURE.md D5):
 *  1. Take the green channel; band-pass 0.5-4 Hz (30-240 bpm).
 *  2. Resample the block's accel onto the PPG timebase (see ppg_block.h) and
 *     reduce motion artifacts (adaptive filter / spectral subtraction with
 *     the accel axes as reference).
 *  3. Peak detection with a refractory period (~250 ms) and adaptive
 *     threshold -> beat times -> beat intervals (kept in the context ring for
 *     resp and core_temp).
 *  4. Every record window (CONFIG_APP_RECORD_PERIOD_MS): HR avg/min/max from
 *     the window's beat intervals.
 *  5. Quality: lowered by the motion flag, by an IR level that says
 *     off-wrist, and by beat-interval irregularity.
 *
 * PLACEHOLDER: publishes 60.00 bpm at each window end if the window had a
 * non-flat, non-saturated PPG signal; otherwise publishes with quality 0
 * (so validation flags it INVALID). No PPG blocks at all -> nothing is
 * published -> the packager marks HR stale.
 *
 * State: one file-static struct hr_ctx (window, beat-interval ring, last
 * valid summary). Only the proc thread calls these functions.
 */

#ifndef APP_PROCESSING_HR_H_
#define APP_PROCESSING_HR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "app/metric.h"
#include "app/ppg_block.h"

/** Opaque: lives in hr.c. */
struct hr_ctx;

/** @brief Reset the context. main, before threads. @return 0. */
int hr_init(void);

/**
 * @brief Feed one PPG block.
 *
 * proc thread; never blocks.
 * @param motion True if motion() flagged this block (lowers quality).
 * @return 1 if a window summary was published (out filled), 0 if not, -errno.
 */
int hr_process_block(const struct ppg_block *blk, bool motion, struct hr_summary *out);

/**
 * @brief Validate a candidate summary against bounds (30-220 bpm), rate of
 *        change versus the last valid summary, and quality.
 *
 * Pure. proc thread. @return METRIC_OK or METRIC_INVALID.
 */
enum metric_status hr_validate(const struct hr_summary *cand, const struct hr_ctx *ctx);

/**
 * @brief Copy the most recent beat intervals (ms), oldest first.
 *
 * proc thread (resp, core_temp). Never blocks.
 * @return number copied (0 until beat detection exists).
 */
size_t hr_get_beat_intervals(uint16_t *out, size_t max);

#endif /* APP_PROCESSING_HR_H_ */
