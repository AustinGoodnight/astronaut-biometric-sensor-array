/*
 * Fuel gauge driver interface (MAX17048). Implemented by
 * src/drivers/max17048.c and src/sim/fake_sensors.c. acq thread only,
 * except the alert callback (ISR).
 *
 * Return codes: 0; -EIO bus error; -ENODEV not present; -ENOSYS stub.
 */

#ifndef APP_DRV_FUEL_GAUGE_DRV_H_
#define APP_DRV_FUEL_GAUGE_DRV_H_

#include <stdint.h>

struct fg_reading {
	uint16_t mv;
	/** State of charge, % (0..100). */
	uint8_t soc_pct;
};

/** ISR context: post EVT_FG_ALERT and return. */
typedef void (*fg_drv_alert_cb_t)(void);

/**
 * @brief Probe, set the ALRT threshold to @p alert_pct, wire the ALRT GPIO to @p cb.
 *
 * acq/main init path; blocks on I2C.
 */
int fg_drv_init(uint8_t alert_pct, fg_drv_alert_cb_t cb);

/** @brief Read voltage and SOC. acq; blocks on I2C. */
int fg_drv_read(struct fg_reading *out);

/** @brief Clear the ALRT flag (after handling EVT_FG_ALERT). acq; blocks on I2C. */
int fg_drv_clear_alert(void);

/** @brief Re-probe and reconfigure (recovery step). acq; blocks on I2C. */
int fg_drv_reset(void);

#endif /* APP_DRV_FUEL_GAUGE_DRV_H_ */
