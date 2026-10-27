/*
 * Record log (ARCHITECTURE.md §9). The storage thread (prio 8) batches
 * records into a 256-byte page buffer (8 records) and writes whole pages
 * through the backend selected by Kconfig (APP_STORAGE_BACKEND_*).
 */

#ifndef APP_STORAGE_STORAGE_H_
#define APP_STORAGE_STORAGE_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#include "app/record.h"

#define STORAGE_PAGE_SIZE        256
#define STORAGE_RECORDS_PER_PAGE (STORAGE_PAGE_SIZE / RECORD_SIZE)

/** @brief Init the backend (mount / find the head). main, before threads. Blocks on flash I/O. */
int storage_init(void);

/** @brief Start the storage thread. main, after inits. */
void storage_start(void);

/**
 * @brief Queue a record for the log.
 *
 * Any thread; never blocks. @return 0, or -ENOMSG if the queue is full
 * (the record is dropped and counted).
 */
int storage_submit(const struct record *rec);

/**
 * @brief Write the partial page now and wait until it's on flash.
 *
 * Supervisor thread only (mode change, power-off). Blocks the CALLER for up
 * to @p timeout. @return 0, -EAGAIN on timeout, -errno on write error.
 */
int storage_flush(k_timeout_t timeout);

/**
 * @brief Read up to @p max records starting at (boot_count, seq). For
 *        backfill and USB log readout.
 *
 * NOT IMPLEMENTED: returns -ENOSYS. (Will be: any thread except storage;
 * blocks on flash reads.)
 */
int storage_read_by_seq(uint16_t boot_count, uint32_t seq, struct record *out, size_t max,
			size_t *n);

#endif /* APP_STORAGE_STORAGE_H_ */
