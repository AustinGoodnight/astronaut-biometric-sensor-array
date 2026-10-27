/*
 * Debug shell: "wrist stats | health | mode <normal|log|raw> | poweroff".
 * Only built with CONFIG_APP_SHELL (debug builds).
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/shell/shell.h>

#include "app/core/health.h"
#include "app/core/mode.h"
#include "app/core/power.h"
#include "app/counters.h"

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (int i = 0; i < CNT_COUNT; i++) {
		shell_print(sh, "%-18s %u", counter_name(i), counter_get(i));
	}
	return 0;
}

static int cmd_health(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	for (int s = 0; s < SENSOR_COUNT; s++) {
		shell_print(sh, "%-11s %s", sensor_name(s), health_state_name(health_get_state(s)));
	}
	shell_print(sh, "packed      0x%04x", health_get_packed());
	return 0;
}

static int cmd_mode(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(sh, "mode %s", mode_name(mode_get()));
		return 0;
	}

	enum app_mode m;

	if (strcmp(argv[1], "normal") == 0) {
		m = MODE_NORMAL;
	} else if (strcmp(argv[1], "log") == 0) {
		m = MODE_LOG_ONLY;
	} else if (strcmp(argv[1], "raw") == 0) {
		m = MODE_RAW_WAVEFORM;
	} else {
		shell_error(sh, "usage: wrist mode <normal|log|raw>");
		return -EINVAL;
	}
	return mode_request(m, MODE_SRC_SHELL);
}

static int cmd_poweroff(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "powering off");
	power_request_off(POWER_OFF_SHELL);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(wrist_cmds,
	SHELL_CMD(stats, NULL, "Reliability counters", cmd_stats),
	SHELL_CMD(health, NULL, "Per-sensor health", cmd_health),
	SHELL_CMD(mode, NULL, "Show or set mode: normal | log | raw", cmd_mode),
	SHELL_CMD(poweroff, NULL, "Run the power-off sequence", cmd_poweroff),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(wrist, &wrist_cmds, "Wrist module", NULL);
