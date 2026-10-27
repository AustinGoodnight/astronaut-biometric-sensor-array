/*
 * Wrist <-> helmet BLE protocol. Shared with the helmet firmware.
 *
 * Keep this header self-contained: fixed-width types, packed structs and
 * #defines only. No Zephyr includes, so the helmet can include it as is.
 *
 * Service and characteristics (ARCHITECTURE.md §8):
 *   data    (notify)        one 32-byte record per notification (record.h)
 *   status  (read, notify)  struct wrist_status
 *   control (write w/ rsp)  [opcode u8][payload]
 *
 * The central must negotiate ATT MTU >= BLE_MIN_ATT_MTU before subscribing
 * to data, or notifications are refused.
 */

#ifndef APP_BLE_PROTO_H_
#define APP_BLE_PROTO_H_

#include <stdint.h>

#define BLE_PROTO_VERSION 1
#define BLE_MIN_ATT_MTU   35 /* 32-byte record + 3-byte ATT header */

/*
 * 128-bit UUIDs: 7a1eXXXX-5b3c-4d2e-9f10-3a6c2b8e4d10, XXXX below.
 * WRIST_UUID_BYTES() expands to the 16 bytes in little-endian (on-air) order.
 */
#define WRIST_UUID_SVC     0x0001
#define WRIST_UUID_DATA    0x0002
#define WRIST_UUID_STATUS  0x0003
#define WRIST_UUID_CONTROL 0x0004

#define WRIST_UUID_BYTES(s)                                                                        \
	0x10, 0x4d, 0x8e, 0x2b, 0x6c, 0x3a, 0x10, 0x9f, 0x2e, 0x4d, 0x3c, 0x5b, ((s) & 0xff),      \
		(((s) >> 8) & 0xff), 0x1e, 0x7a

/* Control opcodes (first byte of a control write). */
#define BLE_OP_TIME_SYNC     0x01 /* struct ble_op_time_sync */
#define BLE_OP_HELMET_STATUS 0x02 /* struct ble_op_helmet_status */
#define BLE_OP_SET_MODE      0x03 /* struct ble_op_set_mode */
#define BLE_OP_BACKFILL_REQ  0x04 /* struct ble_op_backfill_req; NOT IMPLEMENTED yet */
/* 0x05..0x7F reserved. */

/* ATT application errors returned by the control characteristic. */
#define BLE_ATT_ERR_NOT_SUPPORTED 0x80 /* unknown or not-yet-implemented opcode */
#define BLE_ATT_ERR_BAD_VALUE     0x81 /* payload value out of range */
/* A wrong payload length returns the standard ATT error 0x0D (invalid attribute length). */

/* Time-anchor sources (ble_op_time_sync.source, record_time_anchor.source). */
#define BLE_TIME_SRC_HELMET_RTC 0
#define BLE_TIME_SRC_HELMET_GPS 1
#define BLE_TIME_SRC_BASE       2

/* Modes for BLE_OP_SET_MODE and wrist_status.mode. */
#define BLE_MODE_NORMAL       0
#define BLE_MODE_LOG_ONLY     1
#define BLE_MODE_RAW_WAVEFORM 2

struct ble_op_time_sync {
	uint64_t epoch_ms;
	uint8_t source;
} __attribute__((__packed__));

struct ble_op_helmet_status {
	uint8_t lora_link;       /* 0 = down, 1 = up */
	uint8_t helmet_batt_pct;
	uint8_t flags;           /* reserved, 0 */
} __attribute__((__packed__));

struct ble_op_set_mode {
	uint8_t mode; /* BLE_MODE_* */
} __attribute__((__packed__));

struct ble_op_backfill_req {
	uint16_t boot_count;
	uint32_t seq_from;
	uint32_t seq_to;
} __attribute__((__packed__));

/* Status characteristic (read / notify). */
struct wrist_status {
	uint8_t proto_version; /* BLE_PROTO_VERSION */
	uint8_t mode;          /* BLE_MODE_* */
	uint8_t battery_pct;   /* 0xFF = unknown */
	uint8_t flags;         /* bit0 time synced, bit1 sensor fault, bit2 low battery */
	uint16_t health;       /* same packing as record_data.health */
	uint8_t last_cmd_op;
	uint8_t last_cmd_result; /* 0 = OK, else ATT error */
	uint32_t uptime_s;
} __attribute__((__packed__));

#define WRIST_STATUS_FLAG_TIME_SYNCED  0x01
#define WRIST_STATUS_FLAG_SENSOR_FAULT 0x02
#define WRIST_STATUS_FLAG_LOW_BATTERY  0x04

#endif /* APP_BLE_PROTO_H_ */
