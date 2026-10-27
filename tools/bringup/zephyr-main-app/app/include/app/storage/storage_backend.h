/*
 * Storage backend ops (open decision #4). Exactly one backend is compiled
 * in: storage_nor.c (APP_STORAGE_BACKEND_NOR) or storage_sd.c
 * (APP_STORAGE_BACKEND_SD). Called from the storage thread only (init from
 * main).
 */

#ifndef APP_STORAGE_STORAGE_BACKEND_H_
#define APP_STORAGE_STORAGE_BACKEND_H_

#include <stddef.h>
#include <stdint.h>

#include "app/record.h"

struct storage_backend {
	const char *name;
	/** Mount; find the newest entry after a power loss. Blocks on flash I/O. */
	int (*init)(void);
	/**
	 * Append one batch (a whole number of records, at most one page).
	 * Must be power-loss safe: a torn write is detected and discarded at
	 * the next init. Blocks on flash I/O (and on an erase when wrapping).
	 */
	int (*append)(const uint8_t *buf, size_t len);
	/** See storage_read_by_seq(). */
	int (*read_by_seq)(uint16_t boot_count, uint32_t seq, struct record *out, size_t max,
			   size_t *n);
};

/** @brief The compiled-in backend. */
const struct storage_backend *storage_backend_get(void);

#endif /* APP_STORAGE_STORAGE_BACKEND_H_ */
