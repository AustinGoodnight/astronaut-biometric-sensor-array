/*
 * Board power and bus control. Every pin is an optional zephyr,user
 * property: when the board doesn't have it, the call returns -ENOTSUP and
 * the rest of the firmware carries on (e.g. no power-hold on the XIAO).
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/sensors/board.h"

#if defined(CONFIG_APP_FAKE_SENSORS)
#include "app/sim/fake_sensors.h"
#endif

LOG_MODULE_REGISTER(board, CONFIG_APP_LOG_LEVEL);

#define USER_NODE DT_PATH(zephyr_user)

static const struct gpio_dt_spec rail_en[BUS_COUNT] = {
	[BUS_A] = GPIO_DT_SPEC_GET_OR(USER_NODE, rail_a_en_gpios, {0}),
	[BUS_B] = GPIO_DT_SPEC_GET_OR(USER_NODE, rail_b_en_gpios, {0}),
};
static const struct gpio_dt_spec boost_en = GPIO_DT_SPEC_GET_OR(USER_NODE, boost_5v_en_gpios, {0});
static const struct gpio_dt_spec gsr_en = GPIO_DT_SPEC_GET_OR(USER_NODE, gsr_en_gpios, {0});
static const struct gpio_dt_spec flex_en = GPIO_DT_SPEC_GET_OR(USER_NODE, flex_en_gpios, {0});
static const struct gpio_dt_spec power_hold =
	GPIO_DT_SPEC_GET_OR(USER_NODE, power_hold_gpios, {0});

#if !defined(CONFIG_APP_FAKE_SENSORS)
static const struct device *const i2c_bus[BUS_COUNT] = {
	[BUS_A] = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c0)),
	[BUS_B] = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(i2c1)),
};
#endif

static int out_init(const struct gpio_dt_spec *g, gpio_flags_t initial, const char *what)
{
	if (g->port == NULL) {
		return 0; /* not on this board */
	}
	if (!gpio_is_ready_dt(g)) {
		LOG_ERR("%s GPIO not ready", what);
		return -ENODEV;
	}
	return gpio_pin_configure_dt(g, initial);
}

static int out_set(const struct gpio_dt_spec *g, bool on)
{
	if (g->port == NULL) {
		return -ENOTSUP;
	}
	return gpio_pin_set_dt(g, on ? 1 : 0);
}

int board_init(void)
{
	int err = 0;

	err |= out_init(&rail_en[BUS_A], GPIO_OUTPUT_ACTIVE, "rail A");
	err |= out_init(&rail_en[BUS_B], GPIO_OUTPUT_ACTIVE, "rail B");
	err |= out_init(&boost_en, GPIO_OUTPUT_INACTIVE, "boost");
	err |= out_init(&gsr_en, GPIO_OUTPUT_INACTIVE, "gsr en");
	err |= out_init(&flex_en, GPIO_OUTPUT_INACTIVE, "flex en");
	/* Hold power on as early as possible. */
	err |= out_init(&power_hold, GPIO_OUTPUT_ACTIVE, "power hold");

	return err ? -EIO : 0;
}

bool board_rail_present(enum app_bus bus)
{
	return bus < BUS_COUNT && rail_en[bus].port != NULL;
}

int board_rail_set(enum app_bus bus, bool on)
{
	if (bus >= BUS_COUNT) {
		return -EINVAL;
	}
	return out_set(&rail_en[bus], on);
}

int board_boost_set(bool on)
{
	return out_set(&boost_en, on);
}

int board_gsr_en_set(bool on)
{
	return out_set(&gsr_en, on);
}

int board_flex_en_set(bool on)
{
	return out_set(&flex_en, on);
}

int board_power_hold_release(void)
{
	return out_set(&power_hold, false);
}

int board_bus_recover(enum app_bus bus)
{
	if (bus >= BUS_COUNT) {
		return -EINVAL;
	}
#if defined(CONFIG_APP_FAKE_SENSORS)
	return fake_sensors_bus_recover(bus);
#else
	if (i2c_bus[bus] == NULL || !device_is_ready(i2c_bus[bus])) {
		return -ENODEV;
	}
	return i2c_recover_bus(i2c_bus[bus]);
#endif
}
