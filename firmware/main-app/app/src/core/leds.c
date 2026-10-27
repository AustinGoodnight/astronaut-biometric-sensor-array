#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/core/leds.h"

LOG_MODULE_REGISTER(leds, CONFIG_APP_LOG_LEVEL);

static const struct gpio_dt_spec led_gpio[LED_COUNT] = {
	[LED_FAULT] = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0}),
	[LED_HEARTBEAT] = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led1), gpios, {0}),
	[LED_LINK] = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led2), gpios, {0}),
};

static struct k_work_delayable off_work[LED_COUNT];

static void led_off_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	enum led_id led = (enum led_id)(dwork - off_work);

	leds_set(led, false);
}

int leds_init(void)
{
	int ret = 0;

	for (int i = 0; i < LED_COUNT; i++) {
		k_work_init_delayable(&off_work[i], led_off_handler);
		if (led_gpio[i].port == NULL) {
			continue;
		}
		if (!gpio_is_ready_dt(&led_gpio[i])) {
			LOG_ERR("LED %d not ready", i);
			ret = -ENODEV;
			continue;
		}
		(void)gpio_pin_configure_dt(&led_gpio[i], GPIO_OUTPUT_INACTIVE);
	}
	return ret;
}

void leds_set(enum led_id led, bool on)
{
	if (led < LED_COUNT && led_gpio[led].port != NULL) {
		(void)gpio_pin_set_dt(&led_gpio[led], on ? 1 : 0);
	}
}

void leds_pulse(enum led_id led, uint32_t ms)
{
	if (led >= LED_COUNT) {
		return;
	}
	leds_set(led, true);
	(void)k_work_reschedule(&off_work[led], K_MSEC(ms));
}
