/*
 * Analog (SAADC) driver interface for the GSR and flex front ends.
 * Implemented by src/drivers/analog_saadc.c (Zephyr ADC API, channels from
 * zephyr,user io-channels) and src/sim/fake_sensors.c. acq thread only.
 * Front-end power is handled by sensors/board.c, not here.
 *
 * Return codes: 0; -EIO conversion failed; -ENODEV channel missing; -ENOSYS stub.
 */

#ifndef APP_DRV_ANALOG_DRV_H_
#define APP_DRV_ANALOG_DRV_H_

#include <stdint.h>

enum analog_ch {
	ANALOG_CH_GSR = 0,
	ANALOG_CH_FLEX,
	ANALOG_CH_COUNT,
};

/** @brief Set up the ADC channel. acq/main init path; never blocks long. */
int analog_drv_init(enum analog_ch ch);

/**
 * @brief One conversion, in mV at the pin.
 *
 * acq; blocks for the conversion (~tens of µs).
 */
int analog_drv_read_mv(enum analog_ch ch, int32_t *mv);

#endif /* APP_DRV_ANALOG_DRV_H_ */
