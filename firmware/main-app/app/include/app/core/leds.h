/*
 * Status LEDs, driven by health (faults, heartbeat) and ble (link).
 * Mapped to devicetree aliases led0..led2 (XIAO: red, green, blue).
 * Pulses are short (tens of ms) to save power.
 */

#ifndef APP_CORE_LEDS_H_
#define APP_CORE_LEDS_H_

#include <stdbool.h>
#include <stdint.h>

enum led_id {
	LED_FAULT = 0, /* led0: red */
	LED_HEARTBEAT, /* led1: green */
	LED_LINK,      /* led2: blue */
	LED_COUNT,
};

/** @brief Configure the LED GPIOs (all off). main, before threads. @return 0 or -errno. */
int leds_init(void);

/** @brief Turn an LED on or off. Any thread; never blocks. */
void leds_set(enum led_id led, bool on);

/**
 * @brief Turn an LED on for @p ms, then off (system workqueue turns it off).
 *
 * Any thread; never blocks.
 */
void leds_pulse(enum led_id led, uint32_t ms);

#endif /* APP_CORE_LEDS_H_ */
