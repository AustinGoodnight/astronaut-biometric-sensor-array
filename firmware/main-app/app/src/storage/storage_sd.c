/*
 * SD backend (APP_STORAGE_BACKEND_SD): STUB.
 *
 * TODO: SD card on SPIM3 (sdhc-spi-slot), either FATFS with one file per
 * boot (simple to read on a PC) or raw block appends with the same
 * page-per-batch layout as NOR. Must handle power loss mid-write (FATFS:
 * fs_sync() after every page).
 */

#include <zephyr/kernel.h>

#include "app/storage/storage_backend.h"

static int sd_init(void)
{
	return -ENOSYS;
}

static int sd_append(const uint8_t *buf, size_t len)
{
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	return -ENOSYS;
}

static int sd_read_by_seq(uint16_t boot_count, uint32_t seq, struct record *out, size_t max,
			  size_t *n)
{
	ARG_UNUSED(boot_count);
	ARG_UNUSED(seq);
	ARG_UNUSED(out);
	ARG_UNUSED(max);
	*n = 0;
	return -ENOSYS;
}

static const struct storage_backend sd_backend = {
	.name = "sd",
	.init = sd_init,
	.append = sd_append,
	.read_by_seq = sd_read_by_seq,
};

const struct storage_backend *storage_backend_get(void)
{
	return &sd_backend;
}
