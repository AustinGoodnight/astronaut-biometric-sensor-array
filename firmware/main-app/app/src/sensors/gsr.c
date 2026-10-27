/*
 * GSR: Grove-style front end, powered only around each SAADC read
 * (phase 1: power on; phase 2 after the settle timer: read, power off).
 */

#include <zephyr/kernel.h>

#include "app/drv/analog_drv.h"
#include "app/sensors/board.h"
#include "app/sensors/sensors.h"

int gsr_init(void)
{
	return analog_drv_init(ANALOG_CH_GSR);
}

void gsr_power(bool on)
{
	(void)board_gsr_en_set(on);
}

int gsr_read(int32_t *mv)
{
	int err = analog_drv_read_mv(ANALOG_CH_GSR, mv);

	health_report(SENSOR_GSR, err);
	return err;
}
