/* Built only when CONFIG_LSM6DSL is set (xiao_ble/nrf52840/sense); see
 * CMakeLists.txt. The plain-board stubs are in motion.h. */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/logging/log.h>

#include "motion.h"

LOG_MODULE_REGISTER(motion, LOG_LEVEL_INF);

#define IMU_NODE         DT_NODELABEL(lsm6ds3tr_c)
#define IMU_REG_WHO_AM_I 0x0F
#define IMU_WHO_AM_I     0x6A   /* LSM6DSL and LSM6DS3TR-C both answer this */

static const struct device *const imu = DEVICE_DT_GET(IMU_NODE);
static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(IMU_NODE);
static bool imu_ok;

bool motion_init(void)
{
    /* The IMU node is zephyr,deferred-init (Sense overlay), so its driver starts
     * here, well after its power switch came on at boot. */
    int err = device_init(imu);

    imu_ok = device_is_ready(imu);
    if (imu_ok) {
        return true;
    }

    /* The driver's own failure messages are debug-level only, so probe the chip
     * directly to say whether it's power/wiring or the chip itself. */
    uint8_t id;
    int probe = i2c_reg_read_byte_dt(&imu_i2c, IMU_REG_WHO_AM_I, &id);

    if (probe < 0) {
        LOG_ERR("IMU driver init failed (%d) and no ACK from 0x%02x on %s (err %d): "
                "IMU power switch or bus problem", err, imu_i2c.addr, imu_i2c.bus->name, probe);
    } else {
        LOG_ERR("IMU driver init failed (%d); chip answers WHO_AM_I=0x%02x (driver expects 0x%02x)",
                err, id, IMU_WHO_AM_I);
    }
    return false;
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
