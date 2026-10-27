/*
 * Galvanic skin response (publishes chan_gsr, nS).
 *
 * Intended algorithm (TODO):
 *  - Convert the front end's output voltage to skin conductance using the
 *    Grove GSR transfer function (calibrate with known resistors).
 *  - EMA for the tonic level; keep phasic peaks for later analysis.
 *  - Electrode-off detection: conductance near 0 (open circuit) ->
 *    quality 0.
 *
 * PLACEHOLDER: conductance_nS = mV * 10 (not physical), validated.
 *
 * State: file-static struct gsr_ctx. proc thread only.
 */

#ifndef APP_PROCESSING_GSR_PROC_H_
#define APP_PROCESSING_GSR_PROC_H_

#include <stdint.h>

#include "app/metric.h"

struct gsr_ctx;

/** @brief main, before threads. @return 0. */
int gsr_proc_init(void);

/** @brief One sample in mV. proc thread; never blocks. @return 0 or -errno. */
int gsr_proc_process(int32_t mv, uint32_t ts_ms);

/** @brief Bounds 0-100 µS, quality. Pure. proc thread. */
enum metric_status gsr_validate(const struct metric_value *cand, const struct gsr_ctx *ctx);

#endif /* APP_PROCESSING_GSR_PROC_H_ */
