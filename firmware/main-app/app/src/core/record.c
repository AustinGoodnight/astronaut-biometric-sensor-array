/*
 * Record building and emission. seq is a single atomic counter shared by all
 * record types, so (boot_count, seq) is unique and ordered across every
 * record this boot (that is what backfill will key on).
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/crc.h>

#include "app/comms/ble.h"
#include "app/counters.h"
#include "app/record.h"
#include "app/storage/storage.h"

static uint16_t boot_count;
static atomic_t next_seq;
static atomic_t time_synced;

void record_init(uint16_t boot)
{
	boot_count = boot;
	atomic_set(&next_seq, 0);
	atomic_set(&time_synced, 0);
}

void record_begin(struct record *rec, enum record_type type)
{
	memset(rec, 0, sizeof(*rec));
	rec->hdr.record_type = (uint8_t)type;
	rec->hdr.format_version = RECORD_FORMAT_VERSION;
	rec->hdr.boot_count = boot_count;
	rec->hdr.seq = (uint32_t)atomic_inc(&next_seq);
	rec->hdr.uptime_ms = k_uptime_get_32();
}

static uint16_t record_crc(const struct record *rec)
{
	return crc16_itu_t(0xFFFF, (const uint8_t *)rec, offsetof(struct record, crc16));
}

void record_finalize(struct record *rec)
{
	rec->crc16 = record_crc(rec);
}

bool record_crc_ok(const struct record *rec)
{
	return rec->crc16 == record_crc(rec);
}

void record_emit(struct record *rec)
{
	record_finalize(rec);
	/* Both calls are non-blocking and count their own drops. */
	(void)storage_submit(rec);
	ble_submit_record(rec);
}

void record_emit_event(enum record_event_code code, uint8_t arg8, uint16_t arg16,
		       uint32_t arg32)
{
	struct record rec;

	record_begin(&rec, RECORD_TYPE_EVENT);
	rec.event.event_code = (uint8_t)code;
	rec.event.arg8 = arg8;
	rec.event.arg16 = arg16;
	rec.event.arg32 = arg32;
	rec.event.i2c_errors = counters_i2c_errors();
	rec.event.ppg_overflows = counters_ppg_overflows();
	rec.event.records_dropped = counters_records_dropped();
	rec.event.recoveries = counters_recoveries();
	record_emit(&rec);
}

void record_emit_time_anchor(uint64_t epoch_ms, uint8_t source)
{
	struct record rec;

	record_begin(&rec, RECORD_TYPE_TIME_ANCHOR);
	rec.anchor.epoch_ms = epoch_ms;
	rec.anchor.source = source;
	record_emit(&rec);
	atomic_set(&time_synced, 1);
}

bool record_time_synced(void)
{
	return atomic_get(&time_synced) != 0;
}
