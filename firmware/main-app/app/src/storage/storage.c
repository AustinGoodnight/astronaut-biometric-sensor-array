/*
 * Storage thread (prio 8): batches records into a page buffer and appends
 * whole pages through the backend.
 *
 * Why batch: one flash program per 8 records (40 s) instead of one per
 * record, so the NOR is awake far less often. The cost is up to 8 records
 * held in RAM, which is why the supervisor flushes before mode changes and
 * power-off.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "app/core/health.h"
#include "app/core/watchdog.h"
#include "app/counters.h"
#include "app/storage/storage.h"
#include "app/storage/storage_backend.h"

LOG_MODULE_REGISTER(storage, CONFIG_APP_LOG_LEVEL);

#define STORAGE_STACK_SIZE 2048
#define STORAGE_PRIO       8
#define STORAGE_WDT_MS     30000
#define STORAGE_WAIT       K_SECONDS(10)
#define STORAGE_Q_DEPTH    16

enum storage_msg_type {
	STORAGE_MSG_RECORD = 1,
	STORAGE_MSG_FLUSH,
};

struct storage_msg {
	uint8_t type;
	struct record rec;
};

K_MSGQ_DEFINE(storage_q, sizeof(struct storage_msg), STORAGE_Q_DEPTH, 4);
static K_SEM_DEFINE(flush_done, 0, 1);

static const struct storage_backend *backend;
static uint8_t page[STORAGE_PAGE_SIZE];
static size_t page_fill;
static int last_flush_err;

int storage_submit(const struct record *rec)
{
	struct storage_msg msg = {.type = STORAGE_MSG_RECORD, .rec = *rec};

	if (k_msgq_put(&storage_q, &msg, K_NO_WAIT) != 0) {
		counter_inc(CNT_REC_DROP_STORAGE);
		return -ENOMSG;
	}
	return 0;
}

int storage_flush(k_timeout_t timeout)
{
	struct storage_msg msg = {.type = STORAGE_MSG_FLUSH};

	k_sem_reset(&flush_done);
	if (k_msgq_put(&storage_q, &msg, timeout) != 0) {
		return -EAGAIN;
	}
	if (k_sem_take(&flush_done, timeout) != 0) {
		return -EAGAIN;
	}
	return last_flush_err;
}

int storage_read_by_seq(uint16_t boot_count, uint32_t seq, struct record *out, size_t max,
			size_t *n)
{
	*n = 0;
	if (backend == NULL || backend->read_by_seq == NULL) {
		return -ENOSYS;
	}
	return backend->read_by_seq(boot_count, seq, out, max, n);
}

static int write_page(void)
{
	if (page_fill == 0) {
		return 0;
	}

	const struct record *first = (const struct record *)page;
	const struct record *last = (const struct record *)(page + page_fill - RECORD_SIZE);
	int err = backend->append(page, page_fill);

	health_report(SENSOR_STORAGE, err);
	if (err) {
		counter_inc(CNT_FLASH_WRITE_ERR);
		LOG_ERR("append %u B (seq %u..%u) failed: %d", page_fill, first->hdr.seq,
			last->hdr.seq, err);
	} else {
		LOG_DBG("appended %u records (seq %u..%u)", page_fill / RECORD_SIZE,
			first->hdr.seq, last->hdr.seq);
	}
	/* The page is released either way: a stuck backend must not wedge the log. */
	page_fill = 0;
	return err;
}

static void storage_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = app_wdt_add(STORAGE_WDT_MS);
	struct storage_msg msg;

	for (;;) {
		int rc = k_msgq_get(&storage_q, &msg, STORAGE_WAIT);

		app_wdt_feed(wdt);
		if (rc != 0) {
			continue;
		}

		switch (msg.type) {
		case STORAGE_MSG_RECORD:
			memcpy(&page[page_fill], &msg.rec, RECORD_SIZE);
			page_fill += RECORD_SIZE;
			if (page_fill == STORAGE_PAGE_SIZE) {
				(void)write_page();
			}
			break;
		case STORAGE_MSG_FLUSH:
			last_flush_err = write_page();
			k_sem_give(&flush_done);
			break;
		default:
			break;
		}
	}
}

K_THREAD_DEFINE(storage_tid, STORAGE_STACK_SIZE, storage_thread, NULL, NULL, NULL, STORAGE_PRIO,
		0, K_TICKS_FOREVER);

int storage_init(void)
{
	backend = storage_backend_get();

	int err = backend->init();

	health_init_result(SENSOR_STORAGE, err);
	LOG_INF("backend %s: %d", backend->name, err);
	return err;
}

void storage_start(void)
{
	k_thread_name_set(storage_tid, "storage");
	k_thread_start(storage_tid);
}
