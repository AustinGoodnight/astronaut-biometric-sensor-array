/*
 * BLE peripheral (ARCHITECTURE.md §8). The ble thread (prio 9) notifies
 * records, handles control writes (forwarded from the BT RX context), and
 * starts/stops advertising with the mode.
 */

#ifndef APP_COMMS_BLE_H_
#define APP_COMMS_BLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "app/record.h"

/** @brief bt_enable(). main, before threads. Blocks while the stack starts. @return 0 or -errno. */
int ble_init(void);

/** @brief Start the ble thread (advertises if the mode is NORMAL). main, after inits. */
void ble_start(void);

/**
 * @brief Queue a record for notification. Dropped (not counted) when not
 *        connected/subscribed, counted when the queue is full.
 *
 * Any thread; never blocks.
 */
void ble_submit_record(const struct record *rec);

/** @brief True while a central is connected. Any thread; never blocks. */
bool ble_is_connected(void);

/* --- internal: between ble_svc.c (GATT) and ble.c (thread) --- */

/** Longest control write: opcode + largest payload. */
#define BLE_CTRL_MAX_LEN 16

/**
 * @brief Hand a validated control write to the ble thread.
 *
 * BT RX context (GATT write callback). Never blocks.
 * @return 0 or -ENOMSG if the queue is full.
 */
int ble_post_ctrl(const uint8_t *data, uint8_t len);

/** @brief Notify subscription changed (CCC write). BT RX context. */
void ble_set_data_subscribed(bool on);
void ble_set_status_subscribed(bool on);

/** @brief Notify a record on the data characteristic. ble thread. @return 0 or -errno. */
int ble_svc_notify_data(const struct record *rec);

/** @brief Notify the status characteristic. ble thread. @return 0 or -errno. */
int ble_svc_notify_status(void);

/** @brief Record the last control command result for the status characteristic. */
void ble_svc_set_last_cmd(uint8_t op, uint8_t result);

#endif /* APP_COMMS_BLE_H_ */
