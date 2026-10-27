/*
 * Operating mode. Requests are cheap and come from anywhere; the
 * supervisor applies them one at a time, so a mode change can never race a
 * power-off or another mode change.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/core/acq.h"
#include "app/core/mode.h"
#include "app/core/persist.h"
#include "app/core/supervisor.h"
#include "app/events.h"
#include "app/record.h"
#include "app/storage/storage.h"

LOG_MODULE_REGISTER(mode, CONFIG_APP_LOG_LEVEL);

#define MODE_FLUSH_TIMEOUT K_SECONDS(2)
#define NO_REQUEST         (-1)

static atomic_t current = ATOMIC_INIT(MODE_NORMAL);
static atomic_t pending = ATOMIC_INIT(NO_REQUEST);
static atomic_t pending_src;

const char *mode_name(enum app_mode m)
{
	static const char *const n[MODE_COUNT] = {"NORMAL", "LOG_ONLY", "RAW_WAVEFORM", "OFF"};

	return m < MODE_COUNT ? n[m] : "?";
}

int mode_init(void)
{
	uint8_t m = persist_mode();

	atomic_set(&current, m < MODE_OFF ? m : MODE_NORMAL);
	LOG_INF("mode %s", mode_name(mode_get()));
	return 0;
}

enum app_mode mode_get(void)
{
	return (enum app_mode)atomic_get(&current);
}

int mode_request(enum app_mode m, enum mode_source source)
{
	if (m >= MODE_OFF) {
		return -EINVAL; /* power-off goes through power_request_off() */
	}
	atomic_set(&pending_src, source);
	atomic_set(&pending, m);
	supervisor_post(SUP_EVT_MODE);
	return 0;
}

static void publish(enum app_mode m, enum mode_source src)
{
	struct mode_msg msg = {.mode = (uint8_t)m, .source = (uint8_t)src};

	(void)zbus_chan_pub(&chan_mode, &msg, K_MSEC(100));
	acq_post(EVT_MODE);
}

void mode_apply_pending(void)
{
	atomic_val_t req = atomic_set(&pending, NO_REQUEST);
	enum app_mode old = mode_get();

	if (req == NO_REQUEST || req == old || old == MODE_OFF) {
		return;
	}

	enum mode_source src = (enum mode_source)atomic_get(&pending_src);

	LOG_INF("mode %s -> %s", mode_name(old), mode_name(req));
	/* Flush first, so the switch point is on flash before anything changes. */
	(void)storage_flush(MODE_FLUSH_TIMEOUT);

	atomic_set(&current, req);
	(void)persist_set_mode((uint8_t)req);
	record_emit_event(REC_EVT_MODE_CHANGE, (uint8_t)req, (uint16_t)old, src);
	publish((enum app_mode)req, src);
}

void mode_enter_off(void)
{
	atomic_set(&current, MODE_OFF);
	publish(MODE_OFF, MODE_SRC_POWER);
}
