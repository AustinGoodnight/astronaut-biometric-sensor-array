#ifndef MOTION_H
#define MOTION_H

/* Wrist motion from the XIAO Sense's on-board IMU, for motion cancelling.
 *
 * Safe to call on every board. The IMU code is only built for
 * xiao_ble/nrf52840/sense (CONFIG_LSM6DSL). On the plain board these are
 * stubs: motion_init() returns false and motion_read_accel() returns
 * -ENODEV, so callers never need their own #if. */

#include <errno.h>
#include <stdbool.h>

#ifdef CONFIG_LSM6DSL

/* Returns true if the IMU is present and ready. */
bool motion_init(void);
/* True after a successful motion_init(). */
bool motion_available(void);
/* Latest acceleration in m/s^2 (x, y, z). 0 on success, negative errno. */
int motion_read_accel(float xyz[3]);

#else

static inline bool motion_init(void) { return false; }
static inline bool motion_available(void) { return false; }
static inline int motion_read_accel(float xyz[3]) { (void)xyz; return -ENODEV; }

#endif /* CONFIG_LSM6DSL */

#endif /* MOTION_H */
