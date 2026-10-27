/*
 * Flex respiration sensor: resistive divider whose high side is switched by
 * a GPIO, powered only around each SAADC read. Only used with
 * CONFIG_APP_RESP_SRC_FLEX.
 */

#include <zephyr/kernel.h>

#include "app/drv/analog_drv.h"
#include "app/sensors/board.h"
#include "app/sensors/sensors.h"

int flex_init(void)
{
	return analog_drv_init(ANALOG_CH_FLEX);
}

void flex_power(bool on)
{
	(void)board_flex_en_set(on);
}

int flex_read(int32_t *mv)
{
	int err = analog_drv_read_mv(ANALOG_CH_FLEX, mv);

	health_report(SENSOR_FLEX, err);
	return err;
}
