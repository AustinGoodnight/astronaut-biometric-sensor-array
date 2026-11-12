#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "maxim_hr.h"
#include "spo2_algorithm.h"

extern "C" int maxim_calculate_heart_rate(uint32_t *ir_samples, uint32_t *red_samples,
					  int32_t *heart_rate, int8_t *heart_rate_valid)
{
	if (ir_samples == NULL || red_samples == NULL || heart_rate == NULL ||
	    heart_rate_valid == NULL) {
		return -EINVAL;
	}

	int32_t spo2;
	int8_t spo2_valid;

	maxim_heart_rate_and_oxygen_saturation(
		ir_samples, MAXIM_HR_SAMPLE_COUNT, red_samples, &spo2, &spo2_valid,
		heart_rate, heart_rate_valid);

	return 0;
}
