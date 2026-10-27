/*
 * The 32-byte record: the unit of logging and of BLE notification.
 * Packed, little-endian, versioned. Exact layout and field meanings:
 * ARCHITECTURE.md §4. Any layout change bumps RECORD_FORMAT_VERSION and
 * updates ARCHITECTURE.md and the base-station decoder.
 */

#ifndef APP_RECORD_H_
#define APP_RECORD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/toolchain.h>
#include <zephyr/sys/util.h>

#define RECORD_FORMAT_VERSION 1
#define RECORD_SIZE           32
#define RECORD_PAYLOAD_SIZE   18

enum record_type {
	RECORD_TYPE_DATA = 0x01,
	RECORD_TYPE_TIME_ANCHOR = 0x02,
	RECORD_TYPE_EVENT = 0x03,
};

/* DATA validity_mask bits: 1 = field valid AND fresh. */
#define REC_VALID_HR_AVG    BIT(0)
#define REC_VALID_HR_MIN    BIT(1)
#define REC_VALID_HR_MAX    BIT(2)
#define REC_VALID_RESP      BIT(3)
#define REC_VALID_SKIN_TEMP BIT(4)
#define REC_VALID_CORE_TEMP BIT(5)
#define REC_VALID_GSR       BIT(6)
#define REC_VALID_ACTIVITY  BIT(7)
#define REC_VALID_BATTERY   BIT(8)

/* DATA flags bits. */
#define REC_FLAG_MOTION       BIT(0)
#define REC_FLAG_OFF_WRIST    BIT(1)
#define REC_FLAG_CHARGING     BIT(2)
#define REC_FLAG_LOW_BATTERY  BIT(3)
#define REC_FLAG_TIME_SYNCED  BIT(4)
#define REC_FLAG_DATA_DROPPED BIT(5)

enum record_event_code {
	REC_EVT_BOOT = 1,         /* arg32 = hwinfo reset cause, arg16 = wdt channel + 1 (0 = none) */
	REC_EVT_POWER_OFF,        /* arg8 = enum power_off_reason */
	REC_EVT_MODE_CHANGE,      /* arg8 = new mode, arg16 = old mode, arg32 = source */
	REC_EVT_SENSOR_FAULT,     /* arg8 = sensor id, arg16 = new health state */
	REC_EVT_SENSOR_RECOVERED, /* arg8 = sensor id, arg16 = new health state */
	REC_EVT_LOW_BATTERY,      /* arg8 = battery % */
	REC_EVT_COUNTERS,         /* periodic snapshot; counters only */
	REC_EVT_STORAGE_WRAP,
	REC_EVT_CMD_REJECTED,     /* arg8 = BLE opcode, arg16 = ATT error */
};

/* 12-byte header shared by every record type. */
struct record_hdr {
	uint8_t record_type;
	uint8_t format_version;
	uint16_t boot_count;
	/** Monotonic per boot, shared by all record types. */
	uint32_t seq;
	uint32_t uptime_ms;
} __packed;

struct record_data {
	uint16_t validity_mask;
	/** 2 bits per sensor (enum sensor_id order), enum health_state values. */
	uint16_t health;
	uint16_t hr_avg;   /* 0.1 bpm */
	uint8_t hr_min;    /* 1 bpm */
	uint8_t hr_max;    /* 1 bpm */
	uint8_t resp_rate; /* 0.5 breaths/min */
	uint8_t activity;  /* 0..255 */
	int16_t skin_temp; /* 0.01 °C */
	int16_t core_temp; /* 0.01 °C */
	uint16_t gsr;      /* 0.01 µS */
	uint8_t battery;   /* % */
	uint8_t flags;     /* REC_FLAG_* */
} __packed;

struct record_time_anchor {
	/** Unix time (ms) from the helmet; the header's uptime_ms is local time at receipt. */
	uint64_t epoch_ms;
	uint8_t source;
	uint8_t reserved[9];
} __packed;

struct record_event {
	uint8_t event_code;
	uint8_t arg8;
	uint16_t arg16;
	uint32_t arg32;
	/* Saturating snapshots of counters.h. */
	uint16_t i2c_errors;
	uint16_t ppg_overflows;
	uint16_t records_dropped;
	uint16_t recoveries;
	uint16_t reserved;
} __packed;

struct record {
	struct record_hdr hdr;
	union {
		struct record_data data;
		struct record_time_anchor anchor;
		struct record_event event;
		uint8_t raw[RECORD_PAYLOAD_SIZE];
	};
	/** CRC-16/CCITT-FALSE (crc16_itu_t, seed 0xFFFF) over bytes 0..29. */
	uint16_t crc16;
} __packed;

BUILD_ASSERT(sizeof(struct record_hdr) == 12, "record header must be 12 bytes");
BUILD_ASSERT(sizeof(struct record_data) == RECORD_PAYLOAD_SIZE, "data payload size");
BUILD_ASSERT(sizeof(struct record_time_anchor) == RECORD_PAYLOAD_SIZE, "anchor payload size");
BUILD_ASSERT(sizeof(struct record_event) == RECORD_PAYLOAD_SIZE, "event payload size");
BUILD_ASSERT(sizeof(struct record) == RECORD_SIZE, "record must be 32 bytes");
BUILD_ASSERT(offsetof(struct record, crc16) == 30, "crc16 at offset 30");
BUILD_ASSERT(offsetof(struct record, data.validity_mask) == 12, "validity at 12");
BUILD_ASSERT(offsetof(struct record, data.health) == 14, "health at 14");
BUILD_ASSERT(offsetof(struct record, data.hr_avg) == 16, "hr_avg at 16");
BUILD_ASSERT(offsetof(struct record, data.resp_rate) == 20, "resp at 20");
BUILD_ASSERT(offsetof(struct record, data.skin_temp) == 22, "skin_temp at 22");
BUILD_ASSERT(offsetof(struct record, data.gsr) == 26, "gsr at 26");
BUILD_ASSERT(offsetof(struct record, data.flags) == 29, "flags at 29");
BUILD_ASSERT(offsetof(struct record, event.i2c_errors) == 20, "event counters at 20");

/**
 * @brief Set the boot count stamped into every record. Resets seq to 0.
 *
 * Called once by main before any record is built. Never blocks.
 */
void record_init(uint16_t boot_count);

/**
 * @brief Fill a record's header (type, version, boot count, next seq, uptime)
 *        and zero its payload.
 *
 * Any thread; never blocks. Seq is allocated atomically.
 */
void record_begin(struct record *rec, enum record_type type);

/**
 * @brief Compute and store the CRC. Call after the payload is complete.
 *
 * Any thread; never blocks.
 */
void record_finalize(struct record *rec);

/** @return true if @p rec's CRC matches. Any thread; never blocks. */
bool record_crc_ok(const struct record *rec);

/**
 * @brief Finalize and hand a record to storage and BLE.
 *
 * Non-blocking queue puts; a full queue drops the record and counts it.
 * Any thread.
 */
void record_emit(struct record *rec);

/**
 * @brief Build and emit an EVENT record with a counters snapshot.
 *
 * Any thread; never blocks.
 */
void record_emit_event(enum record_event_code code, uint8_t arg8, uint16_t arg16,
		       uint32_t arg32);

/**
 * @brief Build and emit a TIME_ANCHOR record (helmet sent wall-clock time).
 *
 * Any thread; never blocks.
 */
void record_emit_time_anchor(uint64_t epoch_ms, uint8_t source);

/** @return true once a time anchor has been emitted this boot. */
bool record_time_synced(void);

#endif /* APP_RECORD_H_ */
