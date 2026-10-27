/*
 * Skin temperature driver interface: MAX30205 or MAX30208 (open decision
 * #2, CONFIG_APP_SKIN_TEMP_*). Implemented by src/drivers/max3020x.c and
 * src/sim/fake_sensors.c. acq thread only.
 *
 * Return codes: 0; -EIO bus error; -ENODEV not present; -EAGAIN no
 * conversion ready yet; -ENOSYS stub.
 */

#ifndef APP_DRV_TEMP_DRV_H_
#define APP_DRV_TEMP_DRV_H_

#include <stdint.h>

/** @brief Probe, configure for one-shot (shutdown between reads). acq/main; blocks on I2C. */
int temp_drv_init(void);

/**
 * @brief Return the previous conversion and start the next one.
 *
 * Non-blocking by design: a MAX30205 one-shot takes ~50 ms, so each call
 * reads the result triggered by the previous call. The first call after
 * init returns -EAGAIN. acq; blocks only on the I2C transfers.
 *
 * @param mdegc Out: temperature in m°C.
 */
int temp_drv_read(int32_t *mdegc);

/** @brief Re-probe and reconfigure (recovery step). acq; blocks on I2C. */
int temp_drv_reset(void);

#endif /* APP_DRV_TEMP_DRV_H_ */
