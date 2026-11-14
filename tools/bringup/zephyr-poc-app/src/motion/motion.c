/* Built only when CONFIG_LSM6DSL is set (xiao_ble/nrf52840/sense); see
 * CMakeLists.txt. The plain-board stubs are in motion.h. */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "motion.h"

LOG_MODULE_REGISTER(motion, LOG_LEVEL_INF);

static const struct device *const imu = DEVICE_DT_GET(DT_NODELABEL(lsm6ds3tr_c));
static bool imu_ok;

bool motion_init(void)
{
    imu_ok = device_is_ready(imu);
    if (!imu_ok) {
        LOG_ERR("IMU %s not ready: is this really a Sense board?", imu->name);
    }
    return imu_ok;
}

bool motion_available(void)
{
    return imu_ok;
}

int motion_read_accel(float xyz[3])
{
    struct sensor_value v[3];
    int err;

    if (!imu_ok) {
        return -ENODEV;
    }

    err = sensor_sample_fetch_chan(imu, SENSOR_CHAN_ACCEL_XYZ);
    if (err == 0) {
        err = sensor_channel_get(imu, SENSOR_CHAN_ACCEL_XYZ, v);
    }
    if (err < 0) {
        return err;
    }

    for (int i = 0; i < 3; i++) {
        xyz[i] = (float)sensor_value_to_double(&v[i]);
    }
    return 0;
}
