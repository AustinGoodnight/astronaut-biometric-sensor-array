#include <zephyr/kernel.h>

#include "app/drv/temp_drv.h"
#include "app/sensors/sensors.h"

int skin_temp_init(void)
{
	return temp_drv_init();
}

int skin_temp_read(int32_t *mdegc)
{
	int err = temp_drv_read(mdegc);

	/* -EAGAIN (first conversion not ready yet) is not a sensor fault. */
	if (err != -EAGAIN) {
		health_report(SENSOR_SKIN_TEMP, err);
	}
	return err;
}

int skin_temp_reinit(void)
{
	int err = temp_drv_reset();

	health_report(SENSOR_SKIN_TEMP, err);
	return err;
}
