/*
 * Board-level power and bus control: per-bus load switches, 5 V boost,
 * analog front-end enables, power-hold, I2C bus recovery.
 *
 * All pins come from the devicetree zephyr,user node; a missing property
 * means "not on this board" and the call is a no-op returning -ENOTSUP
 * (e.g. power-hold on the XIAO).
 */

#ifndef APP_SENSORS_BOARD_H_
#define APP_SENSORS_BOARD_H_

#include <stdbool.h>

#include "app/core/health.h"

/** @brief Configure all enable GPIOs: rails on, boost and front ends off. main. @return 0 or -errno. */
int board_init(void);

/** @brief True if this board can switch @p bus's rail. */
bool board_rail_present(enum app_bus bus);

/** @brief Switch a bus rail. acq (and power-off). Never blocks. @return 0, -ENOTSUP. */
int board_rail_set(enum app_bus bus, bool on);

/** @brief 5 V boost for the PPG LEDs. acq. Never blocks. @return 0, -ENOTSUP. */
int board_boost_set(bool on);

/** @brief GSR front-end power. acq. Never blocks. @return 0, -ENOTSUP. */
int board_gsr_en_set(bool on);

/** @brief Flex divider high side. acq. Never blocks. @return 0, -ENOTSUP. */
int board_flex_en_set(bool on);

/**
 * @brief Release the power-hold line so the push-button controller cuts power.
 *
 * Supervisor (power-off sequence). Never blocks. @return 0, -ENOTSUP.
 */
int board_power_hold_release(void);

/**
 * @brief I2C bus recovery (9 SCL pulses + STOP) on @p bus.
 *
 * acq only (the bus owner). Blocks for the recovery (< 1 ms).
 * With fake sensors this goes to the fake driver layer.
 */
int board_bus_recover(enum app_bus bus);

#endif /* APP_SENSORS_BOARD_H_ */
