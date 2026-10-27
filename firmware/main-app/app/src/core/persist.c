/*
 * Settings-backed persistence: "app/boot_cnt" and "app/mode".
 * Settings live in NVS on the internal storage_partition.
 */

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "app/core/mode.h"
#include "app/core/persist.h"

LOG_MODULE_REGISTER(persist, CONFIG_APP_LOG_LEVEL);

static uint16_t boot_count;
static uint8_t saved_mode = MODE_NORMAL;
static uint32_t reset_cause;

static int app_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	ssize_t rc;

	if (settings_name_steq(name, "boot_cnt", &next) && next == NULL) {
		if (len != sizeof(boot_count)) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &boot_count, sizeof(boot_count));
		return rc < 0 ? (int)rc : 0;
	}
	if (settings_name_steq(name, "mode", &next) && next == NULL) {
		if (len != sizeof(saved_mode)) {
			return -EINVAL;
		}
		rc = read_cb(cb_arg, &saved_mode, sizeof(saved_mode));
		if (saved_mode >= MODE_OFF) {
			saved_mode = MODE_NORMAL;
		}
		return rc < 0 ? (int)rc : 0;
	}
	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(app, "app", NULL, app_settings_set, NULL, NULL);

int persist_init(void)
{
	int err;

	if (hwinfo_get_reset_cause(&reset_cause) == 0) {
		(void)hwinfo_clear_reset_cause();
	}

	err = settings_subsys_init();
	if (err == 0) {
		err = settings_load_subtree("app");
	}
	if (err) {
		LOG_ERR("settings unavailable (%d): boot count not persisted", err);
		boot_count = 0;
		return err;
	}

	boot_count++;
	if (boot_count == 0) {
		boot_count = 1; /* wrapped; 0 means "settings failed" */
	}
	err = settings_save_one("app/boot_cnt", &boot_count, sizeof(boot_count));
	if (err) {
		LOG_ERR("saving boot count failed (%d)", err);
	}
	LOG_INF("boot #%u, reset cause 0x%08x", boot_count, reset_cause);
	return err;
}

uint16_t persist_boot_count(void)
{
	return boot_count;
}

uint32_t persist_reset_cause(void)
{
	return reset_cause;
}

uint8_t persist_mode(void)
{
	return saved_mode;
}

int persist_set_mode(uint8_t mode)
{
	saved_mode = mode;
	return settings_save_one("app/mode", &saved_mode, sizeof(saved_mode));
}
