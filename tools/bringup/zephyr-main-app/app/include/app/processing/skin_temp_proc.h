/*
 * Skin temperature smoothing + validation (publishes chan_skin_temp, m°C).
 *
 * Intended algorithm (TODO): median-of-3 to reject single spikes, then an
 * EMA (time constant ~30 s).
 *
 * PLACEHOLDER: passes each raw sample through (validated).
 *
 * State: file-static struct skin_temp_ctx. proc thread only.
 */

#ifndef APP_PROCESSING_SKIN_TEMP_PROC_H_
#define APP_PROCESSING_SKIN_TEMP_PROC_H_

#include <stdint.h>

#include "app/metric.h"

struct skin_temp_ctx;

/** @brief main, before threads. @return 0. */
int skin_temp_proc_init(void);

/** @brief One sample in m°C. proc thread; never blocks. @return 0 or -errno. */
int skin_temp_proc_process(int32_t mdegc, uint32_t ts_ms);

/** @brief Bounds 20-42 °C, max 0.5 °C/s, quality. Pure. proc thread. */
enum metric_status skin_temp_validate(const struct metric_value *cand,
				      const struct skin_temp_ctx *ctx);

#endif /* APP_PROCESSING_SKIN_TEMP_PROC_H_ */
