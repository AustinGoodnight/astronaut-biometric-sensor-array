/*
 * Task watchdog. If any channel starves, the callback stores which one in
 * RAM that survives a soft reset (__noinit), then reboots. main reads it at
 * the next boot and puts it in the BOOT event, so a hang is attributable to a
 * thread.
 */

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/task_wdt/task_wdt.h>

#include "app/core/watchdog.h"

LOG_MODULE_REGISTER(wdt, CONFIG_APP_LOG_LEVEL);

#define STARVED_MAGIC 0x57445443u /* "WDTC" */

static __noinit struct {
	uint32_t magic;
	int32_t channel;
} starved;

static void starved_cb(int channel_id, void *user_data)
{
	ARG_UNUSED(user_data);

	starved.magic = STARVED_MAGIC;
	starved.channel = channel_id;
	sys_reboot(SYS_REBOOT_COLD);
}

int app_wdt_init(void)
{
	const struct device *hw = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(wdt0));

	if (hw != NULL && !device_is_ready(hw)) {
		LOG_ERR("hardware WDT not ready; task watchdog without fallback");
		hw = NULL;
	}
	return task_wdt_init(hw);
}

int app_wdt_add(uint32_t timeout_ms)
{
	int ch = task_wdt_add(timeout_ms, starved_cb, NULL);

	if (ch < 0) {
		LOG_ERR("task_wdt_add failed: %d", ch);
	}
	return ch;
}

void app_wdt_feed(int channel)
{
	if (channel >= 0) {
		(void)task_wdt_feed(channel);
	}
}

int app_wdt_last_starved(void)
{
	int ch = -1;

	if (starved.magic == STARVED_MAGIC) {
		ch = starved.channel;
	}
	starved.magic = 0;
	return ch;
}
