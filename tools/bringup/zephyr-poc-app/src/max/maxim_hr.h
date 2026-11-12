#ifndef MAXIM_HR_H
#define MAXIM_HR_H

#include <stdint.h>

#define MAXIM_HR_SAMPLE_COUNT 100

#ifdef __cplusplus
extern "C" {
#endif

int maxim_calculate_heart_rate(uint32_t *ir_samples, uint32_t *red_samples,
			       int32_t *heart_rate, int8_t *heart_rate_valid);

#ifdef __cplusplus
}
#endif

#endif
