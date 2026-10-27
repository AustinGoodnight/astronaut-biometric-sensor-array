/*
 * Wrist GATT service: data (notify), status (read/notify), control (write).
 * UUIDs, opcodes and payloads: include/app/ble_proto.h.
 *
 * Write and CCC callbacks run in the BT RX context: they only validate and
 * hand off to the ble thread; they never block.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/zbus/zbus.h>

#include "app/ble_proto.h"
#include "app/channels.h"
#include "app/comms/ble.h"
#include "app/core/health.h"
#include "app/core/mode.h"
#include "app/record.h"
#include "app/units.h"

static const struct bt_uuid_128 svc_uuid = BT_UUID_INIT_128(WRIST_UUID_BYTES(WRIST_UUID_SVC));
static const struct bt_uuid_128 data_uuid = BT_UUID_INIT_128(WRIST_UUID_BYTES(WRIST_UUID_DATA));
static const struct bt_uuid_128 status_uuid =
	BT_UUID_INIT_128(WRIST_UUID_BYTES(WRIST_UUID_STATUS));
static const struct bt_uuid_128 control_uuid =
	BT_UUID_INIT_128(WRIST_UUID_BYTES(WRIST_UUID_CONTROL));

static atomic_t last_cmd; /* op << 8 | result */

void ble_svc_set_last_cmd(uint8_t op, uint8_t result)
{
	atomic_set(&last_cmd, ((atomic_val_t)op << 8) | result);
}

static void build_status(struct wrist_status *st)
{
	struct metric_value batt;
	uint16_t health = health_get_packed();
	atomic_val_t cmd = atomic_get(&last_cmd);

	st->proto_version = BLE_PROTO_VERSION;
	st->mode = (uint8_t)mode_get();
	st->battery_pct = 0xFF;
	if (zbus_chan_read(&chan_battery, &batt, K_NO_WAIT) == 0 &&
	    metric_is_fresh(&batt, k_uptime_get_32(), BATT_MAX_AGE_MS)) {
		st->battery_pct = rec_batt(batt.value);
	}
	st->flags = 0;
	if (record_time_synced()) {
		st->flags |= WRIST_STATUS_FLAG_TIME_SYNCED;
	}
	if (health_any_failed()) {
		st->flags |= WRIST_STATUS_FLAG_SENSOR_FAULT;
	}
	if (st->battery_pct != 0xFF && st->battery_pct <= 10) {
		st->flags |= WRIST_STATUS_FLAG_LOW_BATTERY;
	}
	st->health = health;
	st->last_cmd_op = (uint8_t)(cmd >> 8);
	st->last_cmd_result = (uint8_t)cmd;
	st->uptime_s = k_uptime_get_32() / 1000;
}

static ssize_t status_read(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
			   uint16_t len, uint16_t offset)
{
	struct wrist_status st;

	build_status(&st);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, &st, sizeof(st));
}

/* Payload length for each implemented opcode; -1 = not supported. */
static int payload_len(uint8_t op)
{
	switch (op) {
	case BLE_OP_TIME_SYNC:
		return sizeof(struct ble_op_time_sync);
	case BLE_OP_HELMET_STATUS:
		return sizeof(struct ble_op_helmet_status);
	case BLE_OP_SET_MODE:
		return sizeof(struct ble_op_set_mode);
	default:
		return -1; /* includes BLE_OP_BACKFILL_REQ: reserved, not implemented */
	}
}

static ssize_t control_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	const uint8_t *data = buf;

	if (offset != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	int expected = payload_len(data[0]);

	if (expected < 0) {
		ble_svc_set_last_cmd(data[0], BLE_ATT_ERR_NOT_SUPPORTED);
		record_emit_event(REC_EVT_CMD_REJECTED, data[0], BLE_ATT_ERR_NOT_SUPPORTED, 0);
		return BT_GATT_ERR(BLE_ATT_ERR_NOT_SUPPORTED);
	}
	if (len != 1 + expected) {
		ble_svc_set_last_cmd(data[0], BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	if (data[0] == BLE_OP_SET_MODE && data[1] > BLE_MODE_RAW_WAVEFORM) {
		ble_svc_set_last_cmd(data[0], BLE_ATT_ERR_BAD_VALUE);
		return BT_GATT_ERR(BLE_ATT_ERR_BAD_VALUE);
	}
	if (ble_post_ctrl(data, (uint8_t)len) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	return len;
}

static void data_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ble_set_data_subscribed(value == BT_GATT_CCC_NOTIFY);
}

static void status_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ble_set_status_subscribed(value == BT_GATT_CCC_NOTIFY);
}

/* Attribute indices used below: 2 = data value, 5 = status value. */
BT_GATT_SERVICE_DEFINE(wrist_svc,
	BT_GATT_PRIMARY_SERVICE(&svc_uuid),
	BT_GATT_CHARACTERISTIC(&data_uuid.uuid, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE, NULL,
			       NULL, NULL),
	BT_GATT_CCC(data_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&status_uuid.uuid, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, status_read, NULL, NULL),
	BT_GATT_CCC(status_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(&control_uuid.uuid, BT_GATT_CHRC_WRITE, BT_GATT_PERM_WRITE, NULL,
			       control_write, NULL),
);

int ble_svc_notify_data(const struct record *rec)
{
	return bt_gatt_notify(NULL, &wrist_svc.attrs[2], rec, sizeof(*rec));
}

int ble_svc_notify_status(void)
{
	struct wrist_status st;

	build_status(&st);
	return bt_gatt_notify(NULL, &wrist_svc.attrs[5], &st, sizeof(st));
}
