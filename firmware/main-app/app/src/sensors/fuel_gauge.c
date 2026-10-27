/*
 * Fuel gauge wrapper (MAX17048). Polled on the slow tick; the ALRT pin
 * posts EVT_FG_ALERT for a low-battery threshold crossing.
 * Also covers the sensors_reinit() dispatch for every sensor.
 */

#include <zephyr/kernel.h>

#include "app/core/acq.h"
#include "app/counters.h"
#include "app/drv/fuel_gauge_drv.h"
#include "app/events.h"
#include "app/sensors/sensors.h"

#define FG_ALERT_PCT 10

/* ISR context. */
static void fg_alert_isr(void)
{
	acq_post(EVT_FG_ALERT);
}

int fuel_gauge_init(void)
{
	return fg_drv_init(FG_ALERT_PCT, fg_alert_isr);
}

int fuel_gauge_read(uint8_t *soc_pct)
{
	struct fg_reading r;
	int err = fg_drv_read(&r);

	health_report(SENSOR_FUEL_GAUGE, err);
	if (err == 0) {
		*soc_pct = r.soc_pct;
	}
	return err;
}

int fuel_gauge_reinit(void)
{
	int err = fg_drv_reset();

	health_report(SENSOR_FUEL_GAUGE, err);
	return err;
}

int sensors_reinit(enum sensor_id sensor)
{
	counter_inc(CNT_REINITS);

	switch (sensor) {
	case SENSOR_PPG:
		return ppg_reinit();
	case SENSOR_ACCEL:
		return accel_reinit();
	case SENSOR_SKIN_TEMP:
		return skin_temp_reinit();
	case SENSOR_FUEL_GAUGE:
		return fuel_gauge_reinit();
	case SENSOR_GSR: {
		int err = gsr_init();

		health_report(SENSOR_GSR, err);
		return err;
	}
	case SENSOR_FLEX: {
		int err = flex_init();

		health_report(SENSOR_FLEX, err);
		return err;
	}
	default:
		return -ENOTSUP;
	}
}
