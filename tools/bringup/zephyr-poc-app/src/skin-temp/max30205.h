/* SPDX-License-Identifier: Apache-2.0 */

#ifndef ZEPHYR_POC_MAX30205_H
#define ZEPHYR_POC_MAX30205_H

#include <stdint.h>

/* Configure the sensor for continuous conversions. */
int max30205_init(void);

/* Read temperature in millidegrees Celsius; call init successfully first. */
int max30205_read_temp_millicelsius(int32_t *temperature_millicelsius);

#endif
