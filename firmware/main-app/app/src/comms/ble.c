/*
 * BLE thread (prio 9, lowest worker). Everything BLE-related that can wait
 * happens here: notifications, control commands, advertising on/off.
 * BT stack callbacks only set flags or queue messages.
 */

#include <string.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/zbus/zbus.h>

#include "app/ble_proto.h"
#include "app/channels.h"
#include "app/comms/ble.h"
#include "app/core/leds.h"
#include "app/core/mode.h"
#include "app/core/watchdog.h"
#include "app/counters.h"
#include "app/record.h"

LOG_MODULE_REGISTER(ble, CONFIG_APP_LOG_LEVEL);

#define BLE_STACK_SIZE    2048
#define BLE_PRIO          9
#define BLE_WDT_MS        30000
#define BLE_WAIT          K_SECONDS(10)
#define BLE_Q_DEPTH       8
#define STATUS_MIN_GAP_MS 5000
/* 1 s advertising interval (units of 0.625 ms). */
#define ADV_INT_MIN       1600
#define ADV_INT_MAX       1760

enum ble_msg_type {
	BLE_MSG_RECORD = 1,
	BLE_MSG_CTRL,
	BLE_MSG_MODE,
	BLE_MSG_ADV_RESTART,
	BLE_MSG_STATUS,
};

struct ble_msg {
	uint8_t type;
	union {
		struct record rec;
		struct {
			uint8_t len;
			uint8_t data[BLE_CTRL_MAX_LEN];
		} ctrl;
	};
};

K_MSGQ_DEFINE(ble_q, sizeof(struct ble_msg), BLE_Q_DEPTH, 4);

static atomic_t connected;
static atomic_t data_subscribed;
static atomic_t status_subscribed;
static atomic_t status_dirty;
static struct bt_conn *current_conn; /* ble thread + conn callbacks (BT RX) */
static bool advertising;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME, sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, WRIST_UUID_BYTES(WRIST_UUID_SVC)),
};

static void post(const struct ble_msg *msg)
{
	if (k_msgq_put(&ble_q, msg, K_NO_WAIT) != 0 && msg->type == BLE_MSG_RECORD) {
		counter_inc(CNT_REC_DROP_BLE);
	}
}

/* --- API used by other modules ------------------------------------------ */

bool ble_is_connected(void)
{
	return atomic_get(&connected) != 0;
}

void ble_submit_record(const struct record *rec)
{
	/* Not connected or not subscribed: the log has it; don't fill the queue. */
	if (!atomic_get(&connected) || !atomic_get(&data_subscribed)) {
		return;
	}

	struct ble_msg msg = {.type = BLE_MSG_RECORD, .rec = *rec};

	post(&msg);
}

int ble_post_ctrl(const uint8_t *data, uint8_t len)
{
	struct ble_msg msg = {.type = BLE_MSG_CTRL};

	if (len > BLE_CTRL_MAX_LEN) {
		return -EINVAL;
	}
	msg.ctrl.len = len;
	memcpy(msg.ctrl.data, data, len);
	return k_msgq_put(&ble_q, &msg, K_NO_WAIT) == 0 ? 0 : -ENOMSG;
}

void ble_set_data_subscribed(bool on)
{
	atomic_set(&data_subscribed, on);
	LOG_INF("data notifications %s", on ? "on" : "off");
}

void ble_set_status_subscribed(bool on)
{
	atomic_set(&status_subscribed, on);
}

/* --- BT callbacks (BT RX context) --------------------------------------- */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_WRN("connection failed (0x%02x)", err);
		return;
	}
	current_conn = bt_conn_ref(conn);
	atomic_set(&connected, 1);
	LOG_INF("connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);

	LOG_INF("disconnected (0x%02x)", reason);
	atomic_set(&connected, 0);
	atomic_set(&data_subscribed, 0);
	atomic_set(&status_subscribed, 0);
	if (current_conn != NULL) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
}

/* The connection object is free again: advertising can restart. */
static void on_recycled(void)
{
	struct ble_msg msg = {.type = BLE_MSG_ADV_RESTART};

	post(&msg);
}

BT_CONN_CB_DEFINE(conn_cbs) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
};

/* zbus listeners run in the publisher's thread: just queue a message. */
static void ble_zbus_listener(const struct zbus_channel *chan)
{
	struct ble_msg msg = {0};

	if (chan == &chan_mode) {
		msg.type = BLE_MSG_MODE;
	} else {
		msg.type = BLE_MSG_STATUS;
	}
	post(&msg);
}

ZBUS_LISTENER_DEFINE(ble_lis, ble_zbus_listener);
ZBUS_CHAN_ADD_OBS(chan_mode, ble_lis, 3);
ZBUS_CHAN_ADD_OBS(chan_health, ble_lis, 3);

/* --- ble thread ------------------------------------------------------------ */

static void adv_start(void)
{
	static const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_INT_MIN, ADV_INT_MAX, NULL);

	if (advertising || ble_is_connected() || mode_get() != MODE_NORMAL) {
		return;
	}
	int err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

	if (err) {
		LOG_ERR("advertising failed: %d", err);
		return;
	}
	advertising = true;
	LOG_INF("advertising as \"%s\"", CONFIG_BT_DEVICE_NAME);
}

static void adv_stop(void)
{
	(void)bt_le_adv_stop();
	advertising = false;
}

static void handle_mode(void)
{
	if (mode_get() == MODE_NORMAL) {
		adv_start();
		return;
	}
	/* LOG_ONLY / OFF: radio off. */
	adv_stop();
	if (current_conn != NULL) {
		(void)bt_conn_disconnect(current_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

static void handle_ctrl(const uint8_t *data, uint8_t len)
{
	ARG_UNUSED(len); /* already validated in ble_svc.c */

	uint8_t op = data[0];
	const uint8_t *p = &data[1];

	switch (op) {
	case BLE_OP_TIME_SYNC: {
		uint64_t epoch_ms = sys_get_le64(p);

		LOG_INF("time sync: %llu ms (source %u)", epoch_ms, p[8]);
		record_emit_time_anchor(epoch_ms, p[8]);
		break;
	}
	case BLE_OP_HELMET_STATUS: {
		struct link_msg link = {
			.lora_link = p[0],
			.helmet_batt_pct = p[1],
			.flags = p[2],
			.ts_ms = k_uptime_get_32(),
		};

		(void)zbus_chan_pub(&chan_link, &link, K_MSEC(10));
		break;
	}
	case BLE_OP_SET_MODE:
		(void)mode_request((enum app_mode)p[0], MODE_SRC_BLE);
		break;
	default:
		break;
	}
	ble_svc_set_last_cmd(op, 0);
	atomic_set(&status_dirty, 1);
}

static void maybe_notify_status(void)
{
	static int64_t last_ms;
	int64_t now = k_uptime_get();

	if (!atomic_get(&status_dirty) || !atomic_get(&status_subscribed) ||
	    now - last_ms < STATUS_MIN_GAP_MS) {
		return;
	}
	if (ble_svc_notify_status() == 0) {
		atomic_set(&status_dirty, 0);
		last_ms = now;
	}
}

static void ble_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = app_wdt_add(BLE_WDT_MS);
	struct ble_msg msg;

	adv_start();

	for (;;) {
		int rc = k_msgq_get(&ble_q, &msg, BLE_WAIT);

		app_wdt_feed(wdt);
		if (rc == 0) {
			switch (msg.type) {
			case BLE_MSG_RECORD:
				if (atomic_get(&data_subscribed) &&
				    ble_svc_notify_data(&msg.rec) != 0) {
					counter_inc(CNT_REC_DROP_BLE);
				}
				break;
			case BLE_MSG_CTRL:
				handle_ctrl(msg.ctrl.data, msg.ctrl.len);
				break;
			case BLE_MSG_MODE:
				handle_mode();
				atomic_set(&status_dirty, 1);
				break;
			case BLE_MSG_ADV_RESTART:
				advertising = false;
				adv_start();
				break;
			case BLE_MSG_STATUS:
				atomic_set(&status_dirty, 1);
				break;
			default:
				break;
			}
		}
		maybe_notify_status();
	}
}

K_THREAD_DEFINE(ble_tid, BLE_STACK_SIZE, ble_thread, NULL, NULL, NULL, BLE_PRIO, 0,
		K_TICKS_FOREVER);

int ble_init(void)
{
	int err = bt_enable(NULL);

	if (err) {
		LOG_ERR("bt_enable failed: %d", err);
	}
	return err;
}

void ble_start(void)
{
	k_thread_name_set(ble_tid, "ble");
	k_thread_start(ble_tid);
}
