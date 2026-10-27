/*
 * Power button and power-off sequence.
 *
 * The button goes through the input subsystem: gpio-keys -> the
 * zephyr,input-longpress node, which emits INPUT_KEY_POWER after a 3 s
 * hold and INPUT_KEY_A for a short press.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/poweroff.h>

#include "app/core/mode.h"
#include "app/core/packager.h"
#include "app/core/power.h"
#include "app/core/supervisor.h"
#include "app/record.h"
#include "app/sensors/board.h"
#include "app/storage/storage.h"

LOG_MODULE_REGISTER(power, CONFIG_APP_LOG_LEVEL);

#define POWER_FLUSH_TIMEOUT K_SECONDS(2)
#define POWER_HOLD_WAIT_MS  500

#define LONGPRESS_NODE DT_NODELABEL(longpress)
#define BUTTON_NODE    DT_NODELABEL(power_button)

static atomic_t off_reason;

#if DT_NODE_HAS_STATUS_OKAY(BUTTON_NODE)
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
#endif

void power_request_off(enum power_off_reason reason)
{
	atomic_set(&off_reason, reason);
	supervisor_post(SUP_EVT_POWER_OFF);
}

#if DT_NODE_HAS_STATUS_OKAY(LONGPRESS_NODE)
/* Input thread context: only post requests. */
static void button_cb(struct input_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	if (evt->type != INPUT_EV_KEY || evt->value == 0) {
		return;
	}
	if (evt->code == INPUT_KEY_POWER) {
		power_request_off(POWER_OFF_BUTTON);
	} else if (evt->code == INPUT_KEY_A) {
		/* TODO(open decision #5): mode-selection button pattern, then
		 * mode_request(..., MODE_SRC_BUTTON).
		 */
		LOG_INF("short press (mode UX not decided)");
	}
}

INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(LONGPRESS_NODE), button_cb, NULL);
#endif

int power_init(void)
{
	return 0;
}

void power_off_sequence(void)
{
	enum power_off_reason reason = (enum power_off_reason)atomic_get(&off_reason);

	LOG_WRN("power-off (reason %d)", reason);

	/* 1. Stop producing: acq stops the sensors, the packager stops. */
	packager_set_enabled(false);
	mode_enter_off();

	/* 2. Last words in the log. */
	record_emit_event(REC_EVT_POWER_OFF, (uint8_t)reason, 0, 0);
	record_emit_event(REC_EVT_COUNTERS, 0, 0, 0);

	/* 3. Make sure they're on flash. */
	int err = storage_flush(POWER_FLUSH_TIMEOUT);

	if (err) {
		LOG_ERR("flush before power-off failed: %d", err);
	}

	/* 4. Let the push-button controller cut power. */
	if (board_power_hold_release() == 0) {
		k_msleep(POWER_HOLD_WAIT_MS);
		LOG_WRN("still powered after releasing power-hold");
	}

	/* 5. Fallback (USB power, or no power-hold on this board): System OFF,
	 * woken by the button.
	 */
#if DT_NODE_HAS_STATUS_OKAY(BUTTON_NODE)
	(void)gpio_pin_interrupt_configure_dt(&button, GPIO_INT_LEVEL_ACTIVE);
#endif
	LOG_PANIC();
	sys_poweroff();
}
