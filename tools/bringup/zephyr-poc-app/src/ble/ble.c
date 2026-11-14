#include "ble.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <bluetooth/services/nus.h>

LOG_MODULE_REGISTER(poc_ble, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

/* Advertising packet: flags + the 128-bit NUS UUID (3 + 18 = 21 of 31 bytes).
 * The dashboard filters on this UUID, so it must be here. */
static const struct bt_data ad[] = {
    BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
    BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

/* Scan response: the name, which doesn't fit next to the UUID above. */
static const struct bt_data sd[] = {
    BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

/* Connectable, 100-150 ms interval: quick to find during bring-up. */
static const struct bt_le_adv_param adv_param =
    BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, BT_GAP_ADV_FAST_INT_MIN_2,
                         BT_GAP_ADV_FAST_INT_MAX_2, NULL);

static struct k_work adv_work;

static void adv_work_handler(struct k_work *work)
{
    ARG_UNUSED(work);

    int err = bt_le_adv_start(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));

    if (err && err != -EALREADY) {
        LOG_ERR("Advertising failed to start: %d", err);
        return;
    }
    LOG_INF("Advertising as \"%s\"", DEVICE_NAME);
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
    char addr[BT_ADDR_LE_STR_LEN];

    bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
    if (err) {
        LOG_WRN("Connection to %s failed: 0x%02x", addr, err);
        return;
    }
    LOG_INF("Connected: %s", addr);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
    ARG_UNUSED(conn);
    LOG_INF("Disconnected (reason 0x%02x)", reason);
}

/* Called once the connection object is freed; only then can a new
 * connectable advertiser start, so restart advertising from here. */
static void on_recycled(void)
{
    k_work_submit(&adv_work);
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = on_connected,
    .disconnected = on_disconnected,
    .recycled = on_recycled,
};

/* Set while a central has notifications on; reset so the next first send logs. */
static bool notify_enabled;
static bool sent_since_enable;

static void on_send_enabled(enum bt_nus_send_status status)
{
    notify_enabled = (status == BT_NUS_SEND_STATUS_ENABLED);
    sent_since_enable = false;
    LOG_INF("Dashboard %s notifications", notify_enabled ? "enabled" : "disabled");
}

static struct bt_nus_cb nus_cb = {
    .send_enabled = on_send_enabled,
};

int ble_send(const uint8_t *data, uint16_t len)
{
    /* NULL conn: notify every connected central that enabled notifications. */
    int err = bt_nus_send(NULL, data, len);

    if (err == -ENOTCONN) {
        return 0;
    }
    if (err) {
        LOG_WRN("NUS send failed: %d", err);
    } else if (notify_enabled && !sent_since_enable) {
        sent_since_enable = true;
        LOG_INF("First payload sent to dashboard (%u bytes)", len);
    }
    return err;
}

int ble_init(void)
{
    int err = bt_enable(NULL);

    if (err) {
        LOG_ERR("bt_enable failed: %d", err);
        return err;
    }
    LOG_INF("Bluetooth enabled");

    err = bt_nus_init(&nus_cb);
    if (err) {
        LOG_ERR("bt_nus_init failed: %d", err);
        return err;
    }

    k_work_init(&adv_work, adv_work_handler);
    k_work_submit(&adv_work);
    return 0;
}
