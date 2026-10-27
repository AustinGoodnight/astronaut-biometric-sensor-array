/*
 * Fixed-point scale factors: the ONLY place they are defined.
 *
 * Two layers:
 *  - metric scale: what struct metric_value.value means on zbus (generous
 *    resolution, int32).
 *  - record encoding: the narrower fields in the 32-byte record
 *    (ARCHITECTURE.md §4.2). The rec_* helpers convert and saturate.
 */

#ifndef APP_UNITS_H_
#define APP_UNITS_H_

#include <stdint.h>

/* --- metric scales (struct metric_value.value) --- */
#define METRIC_HR_PER_BPM       100   /* 0.01 bpm */
#define METRIC_RESP_PER_BRPM    100   /* 0.01 breaths/min */
#define METRIC_TEMP_PER_DEGC    1000  /* 0.001 °C (m°C) */
#define METRIC_GSR_PER_US       1000  /* nS (0.001 µS) */
#define METRIC_ACTIVITY_MAX     1000  /* activity index 0..1000 */
#define METRIC_BATT_PER_PCT     1     /* 1 % */

/* --- record encodings --- */
#define REC_HR_AVG_PER_BPM      10    /* uint16, 0.1 bpm */
#define REC_HR_MINMAX_PER_BPM   1     /* uint8, 1 bpm */
#define REC_RESP_PER_BRPM       2     /* uint8, 0.5 breaths/min */
#define REC_TEMP_PER_DEGC       100   /* int16, 0.01 °C */
#define REC_GSR_PER_US          100   /* uint16, 0.01 µS */
#define REC_ACTIVITY_MAX        255   /* uint8 */

/* --- maximum age before the packager marks a metric stale (ms) --- */
#define HR_MAX_AGE_MS           7500
#define RESP_MAX_AGE_MS         15000
#define SKIN_TEMP_MAX_AGE_MS    30000
#define CORE_TEMP_MAX_AGE_MS    15000
#define GSR_MAX_AGE_MS          5000
#define ACTIVITY_MAX_AGE_MS     7500
#define BATT_MAX_AGE_MS         90000

static inline int32_t units_clamp(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static inline uint16_t rec_hr_avg(int32_t metric)
{
	return (uint16_t)units_clamp(metric / (METRIC_HR_PER_BPM / REC_HR_AVG_PER_BPM), 0,
				     UINT16_MAX);
}

static inline uint8_t rec_hr_minmax(int32_t metric)
{
	return (uint8_t)units_clamp(metric / METRIC_HR_PER_BPM, 0, UINT8_MAX);
}

static inline uint8_t rec_resp(int32_t metric)
{
	return (uint8_t)units_clamp(metric / (METRIC_RESP_PER_BRPM / REC_RESP_PER_BRPM), 0,
				    UINT8_MAX);
}

static inline int16_t rec_temp(int32_t metric)
{
	return (int16_t)units_clamp(metric / (METRIC_TEMP_PER_DEGC / REC_TEMP_PER_DEGC),
				    INT16_MIN, INT16_MAX);
}

static inline uint16_t rec_gsr(int32_t metric)
{
	return (uint16_t)units_clamp(metric / (METRIC_GSR_PER_US / REC_GSR_PER_US), 0,
				     UINT16_MAX);
}

static inline uint8_t rec_activity(int32_t metric)
{
	return (uint8_t)units_clamp(metric * REC_ACTIVITY_MAX / METRIC_ACTIVITY_MAX, 0,
				    REC_ACTIVITY_MAX);
}

static inline uint8_t rec_batt(int32_t metric)
{
	return (uint8_t)units_clamp(metric / METRIC_BATT_PER_PCT, 0, 100);
}

#endif /* APP_UNITS_H_ */
