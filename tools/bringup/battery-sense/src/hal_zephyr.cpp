// nRF52820 backend (and any other Zephyr board). Compiled only by the Zephyr
// build. Pins come from the devicetree overlay in zephyr/boards/, not from
// this file: i2c0 for SCL/SDA, and zephyr,user select-gpios/status-gpios.
#ifdef __ZEPHYR__

#include <stdarg.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include "battery_test.h"
#include "hal.h"

#define USER_NODE DT_PATH(zephyr_user)

static const struct device *const i2c = DEVICE_DT_GET(DT_NODELABEL(i2c0));
static const struct gpio_dt_spec selectPin = GPIO_DT_SPEC_GET(USER_NODE, select_gpios);
static const struct gpio_dt_spec statusPin = GPIO_DT_SPEC_GET(USER_NODE, status_gpios);

bool hal_init() {
  if (!device_is_ready(i2c) || !gpio_is_ready_dt(&selectPin) ||
      !gpio_is_ready_dt(&statusPin)) {
    return false;
  }
  if (i2c_configure(i2c, I2C_SPEED_SET(I2C_SPEED_STANDARD) | I2C_MODE_CONTROLLER) != 0) {
    return false;
  }
  // Flags (pulls etc.) come from the overlay; STATUS must have none.
  return gpio_pin_configure_dt(&selectPin, GPIO_OUTPUT_INACTIVE) == 0 &&
         gpio_pin_configure_dt(&statusPin, GPIO_INPUT) == 0;
}

bool hal_i2c_probe(uint8_t addr) {
  // Zero-length writes aren't supported by every Zephyr I2C driver, so probe
  // with a 1-byte read instead.
  uint8_t dummy;
  return i2c_read(i2c, &dummy, 1, addr) == 0;
}

bool hal_i2c_write(uint8_t addr, const uint8_t *buf, size_t len) {
  return i2c_write(i2c, buf, len, addr) == 0;
}

bool hal_i2c_write_read(uint8_t addr, const uint8_t *wbuf, size_t wlen,
                        uint8_t *rbuf, size_t rlen) {
  return i2c_write_read(i2c, addr, wbuf, wlen, rbuf, rlen) == 0;
}

void hal_set_select(bool high) { gpio_pin_set_dt(&selectPin, high ? 1 : 0); }

bool hal_read_status() { return gpio_pin_get_dt(&statusPin) == 1; }

void hal_delay_ms(uint32_t ms) { k_msleep(ms); }

uint32_t hal_millis() { return k_uptime_get_32(); }

void hal_printf(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  vprintk(fmt, args);
  va_end(args);
}

int main(void) {
  k_msleep(1000); // give the serial monitor a moment to connect
  battery_test_setup();
  while (true) battery_test_loop();
  return 0;
}

#endif // __ZEPHYR__
