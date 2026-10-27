/*
 * Activity level and motion flag from the accelerometer (publishes
 * chan_activity, 0..METRIC_ACTIVITY_MAX).
 *
 * Intended algorithm (TODO):
 *  - |a| per sample, high-pass (remove gravity), sum of absolute values
 *    over the record window -> activity counts, scaled to 0..1000.
 *  - Motion flag per block when short-term energy exceeds a threshold;
 *    hr uses it to lower quality / gate artifact removal.
 *
 * PLACEHOLDER: publishes activity 0 (quality 100) at each window end if the
 * window contained accel samples; motion flag always false. No accel data
 * -> nothing published -> stale.
 *
 * State: file-static struct motion_ctx. proc thread only.
 */

#ifndef APP_PROCESSING_MOTION_H_
#define APP_PROCESSING_MOTION_H_

#include <stdbool.h>

#include "app/metric.h"
#include "app/ppg_block.h"

struct motion_ctx;

/** @brief main, before threads. @return 0. */
int motion_init(void);

/**
 * @brief Feed one block's accel samples. Runs before hr on every block.
 *
 * proc thread; never blocks.
 * @param motion Out: motion flag for this block.
 * @return 0 or -errno.
 */
int motion_process_block(const struct ppg_block *blk, bool *motion);

/** @brief Bounds 0..1000; quality. Pure. proc thread. */
enum metric_status motion_validate(const struct metric_value *cand, const struct motion_ctx *ctx);

#endif /* APP_PROCESSING_MOTION_H_ */
