/*
 * Estimated core temperature from the heart-rate series (publishes
 * chan_core_temp, m°C).
 *
 * Intended algorithm (TODO): ECTemp-style 1-D Kalman filter (Buller et al.).
 *  - State: core temp CT. Process model: CT_k = CT_{k-1} + w, w ~ N(0, gamma^2).
 *  - Observation: HR_k = f(CT_k) + v, v ~ N(0, sigma^2), with f a quadratic
 *    fitted in the literature (linearised around the current estimate for the
 *    Kalman gain).
 *  - Initial CT = 37.1 °C, initial variance 0 (or from skin temp).
 *  - One update per record window, on valid HR only; when HR is invalid,
 *    predict only and let the variance grow (lower the quality).
 *  - State lives for hours in the context; it's reset only at boot.
 *
 * PLACEHOLDER: publishes 37.00 °C, quality 50, when the window's HR is
 * valid; quality 0 (-> INVALID) when it isn't.
 *
 * State: file-static struct core_temp_ctx. proc thread only.
 */

#ifndef APP_PROCESSING_CORE_TEMP_H_
#define APP_PROCESSING_CORE_TEMP_H_

#include "app/metric.h"

struct core_temp_ctx;

/** @brief main, before threads. @return 0. */
int core_temp_init(void);

/**
 * @brief One update with the window's HR summary (valid or not).
 *
 * proc thread; never blocks. @return 0 or -errno.
 */
int core_temp_update(const struct hr_summary *hr);

/** @brief Bounds 35-41 °C, rate of change, quality. Pure. proc thread. */
enum metric_status core_temp_validate(const struct metric_value *cand,
				      const struct core_temp_ctx *ctx);

#endif /* APP_PROCESSING_CORE_TEMP_H_ */
