/*
 * SAADC reads for the GSR and flex front ends (implements analog_drv.h). STUB.
 * Channels come from the zephyr,user io-channels ("gsr", "flex").
 *
 * TODO:
 *  init: adc_is_ready_dt(); adc_channel_setup_dt().
 *  read: adc_sequence_init_dt(); adc_read_dt(); adc_raw_to_millivolts_dt().
 *  (The SAADC suspends between reads via zephyr,pm-device-runtime-auto.)
 */

#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/kernel.h>

#include "app/drv/analog_drv.h"

#define USER_NODE DT_PATH(zephyr_user)

static const struct adc_dt_spec chans[ANALOG_CH_COUNT] = {
	[ANALOG_CH_GSR] = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, gsr),
	[ANALOG_CH_FLEX] = ADC_DT_SPEC_GET_BY_NAME(USER_NODE, flex),
};

int analog_drv_init(enum analog_ch ch)
{
	if (ch >= ANALOG_CH_COUNT) {
		return -EINVAL;
	}
	if (!adc_is_ready_dt(&chans[ch])) {
		return -ENODEV;
	}
	return -ENOSYS;
}

int analog_drv_read_mv(enum analog_ch ch, int32_t *mv)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(mv);
	return -ENOSYS;
}
