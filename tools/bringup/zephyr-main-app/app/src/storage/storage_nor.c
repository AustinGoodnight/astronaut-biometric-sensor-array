/*
 * NOR backend (APP_STORAGE_BACKEND_NOR): STUB.
 *
 * TODO (ARCHITECTURE.md §9):
 *  - init: fcb_init() on FIXED_PARTITION_ID(log_partition) with 64 KB
 *    logical sectors (static struct flash_sector[]); FCB discards a torn
 *    last entry on mount.
 *  - append: fcb_append(len) -> flash_area_write() -> fcb_append_finish();
 *    on -ENOSPC, fcb_rotate() (erases the oldest 64 KB sector), emit
 *    REC_EVT_STORAGE_WRAP, retry.
 *  - read_by_seq: binary-search sectors by the first record's
 *    (boot_count, seq), then walk entries with fcb_getnext().
 *  - Check record_crc_ok() on every record read back.
 *
 * For now it logs what it would write, so the pipeline is visible end to end.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/record.h"
#include "app/storage/storage_backend.h"

LOG_MODULE_REGISTER(storage_nor, CONFIG_APP_LOG_LEVEL);

static uint32_t pages_written;

static int nor_init(void)
{
	LOG_WRN("NOR backend is a stub: records are not persisted");
	return 0;
}

static int nor_append(const uint8_t *buf, size_t len)
{
	const struct record *first = (const struct record *)buf;
	const struct record *last = (const struct record *)(buf + len - RECORD_SIZE);

	pages_written++;
	LOG_INF("[stub] would write page %u: %u records, seq %u..%u", pages_written,
		len / RECORD_SIZE, first->hdr.seq, last->hdr.seq);
	return 0;
}

static int nor_read_by_seq(uint16_t boot_count, uint32_t seq, struct record *out, size_t max,
			   size_t *n)
{
	ARG_UNUSED(boot_count);
	ARG_UNUSED(seq);
	ARG_UNUSED(out);
	ARG_UNUSED(max);
	*n = 0;
	return -ENOSYS;
}

static const struct storage_backend nor_backend = {
	.name = "nor",
	.init = nor_init,
	.append = nor_append,
	.read_by_seq = nor_read_by_seq,
};

const struct storage_backend *storage_backend_get(void)
{
	return &nor_backend;
}
