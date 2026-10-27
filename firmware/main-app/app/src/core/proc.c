/*
 * Processing thread (prio 5). Runs the processing modules on data from acq.
 * Modules keep their own state in file-static contexts and are only ever
 * called from this thread, so they need no locks.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/zbus/zbus.h>

#include "app/channels.h"
#include "app/core/proc.h"
#include "app/core/watchdog.h"
#include "app/counters.h"
#include "app/processing/core_temp.h"
#include "app/processing/gsr_proc.h"
#include "app/processing/hr.h"
#include "app/processing/motion.h"
#include "app/processing/resp.h"
#include "app/processing/skin_temp_proc.h"
#include "app/processing/validate.h"
#include "app/units.h"

LOG_MODULE_REGISTER(proc, CONFIG_APP_LOG_LEVEL);

#define PROC_STACK_SIZE 3072
#define PROC_PRIO       5
#define PROC_WDT_MS     5000
#define PROC_WAIT       K_MSEC(1000)
#define PROC_Q_DEPTH    16

K_MSGQ_DEFINE(proc_q, sizeof(struct acq_msg), PROC_Q_DEPTH, 4);

int proc_submit(const struct acq_msg *msg)
{
	return k_msgq_put(&proc_q, msg, K_NO_WAIT) == 0 ? 0 : -ENOMSG;
}

/* Battery needs no processing: validate and publish. */
static void battery_publish(int32_t pct, uint32_t ts_ms)
{
	static const struct validate_limits lim = {
		.min = 0, .max = 100 * METRIC_BATT_PER_PCT, .max_rate_per_s = 0, .min_quality = 0,
	};
	struct metric_value m = {.value = pct * METRIC_BATT_PER_PCT, .ts_ms = ts_ms, .quality = 100};

	m.status = validate_metric(&m, NULL, &lim);
	if (zbus_chan_pub(&chan_battery, &m, K_MSEC(5)) != 0) {
		counter_inc(CNT_PUBLISH_FAIL);
	}
}

static void handle_block(struct ppg_block *blk)
{
	bool motion = false;
	struct hr_summary hr;

	/* Order matters: motion flags the block before hr uses it. */
	(void)motion_process_block(blk, &motion);

	if (blk->ppg_n > 0) {
		if (hr_process_block(blk, motion, &hr) > 0) {
			/* One core-temp update per HR window (valid or not). */
			(void)core_temp_update(&hr);
		}
		(void)resp_process_block(blk);
	}
	ppg_block_free(blk);
}

static void proc_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	int wdt = app_wdt_add(PROC_WDT_MS);
	struct acq_msg msg;

	for (;;) {
		int rc = k_msgq_get(&proc_q, &msg, PROC_WAIT);

		app_wdt_feed(wdt);
		if (rc != 0) {
			continue;
		}

		switch (msg.type) {
		case ACQ_MSG_PPG_BLOCK:
			handle_block(msg.blk);
			break;
		case ACQ_MSG_SKIN_TEMP:
			(void)skin_temp_proc_process(msg.sample.value, msg.sample.ts_ms);
			break;
		case ACQ_MSG_GSR:
			(void)gsr_proc_process(msg.sample.value, msg.sample.ts_ms);
			break;
		case ACQ_MSG_FLEX:
			(void)resp_process_flex(msg.sample.value, msg.sample.ts_ms);
			break;
		case ACQ_MSG_BATTERY:
			battery_publish(msg.sample.value, msg.sample.ts_ms);
			break;
		default:
			LOG_ERR("unknown msg type %u", msg.type);
			break;
		}
	}
}

K_THREAD_DEFINE(proc_tid, PROC_STACK_SIZE, proc_thread, NULL, NULL, NULL, PROC_PRIO,
		K_FP_REGS, K_TICKS_FOREVER);

int proc_init(void)
{
	int err = 0;

	err |= motion_init();
	err |= hr_init();
	err |= resp_init();
	err |= core_temp_init();
	err |= skin_temp_proc_init();
	err |= gsr_proc_init();
	return err ? -EINVAL : 0;
}

void proc_start(void)
{
	k_thread_name_set(proc_tid, "proc");
	k_thread_start(proc_tid);
}
