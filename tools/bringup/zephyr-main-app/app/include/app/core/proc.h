/*
 * Processing thread (prio 5). Receives acq_msg from acq and runs the
 * processing modules, which validate and publish to zbus.
 */

#ifndef APP_CORE_PROC_H_
#define APP_CORE_PROC_H_

#include <stdint.h>

#include "app/ppg_block.h"

enum acq_msg_type {
	ACQ_MSG_PPG_BLOCK = 1, /* blk: PPG + accel (ppg_n may be 0: accel only) */
	ACQ_MSG_SKIN_TEMP,     /* value: m°C */
	ACQ_MSG_GSR,           /* value: mV at the ADC pin */
	ACQ_MSG_FLEX,          /* value: mV at the ADC pin */
	ACQ_MSG_BATTERY,       /* value: % state of charge */
};

struct acq_msg {
	uint8_t type; /* enum acq_msg_type */
	union {
		/** Owned by proc from here; freed with ppg_block_free(). */
		struct ppg_block *blk;
		struct {
			int32_t value;
			uint32_t ts_ms;
		} sample;
	};
};

/** @brief Init every processing module. main, before threads. @return 0 or -errno. */
int proc_init(void);

/** @brief Start the proc thread. main, after all inits. */
void proc_start(void);

/**
 * @brief Queue a message for proc.
 *
 * acq thread. Never blocks.
 * @return 0, or -ENOMSG if the queue is full (caller drops, counts, and
 *         frees the block if there is one).
 */
int proc_submit(const struct acq_msg *msg);

#endif /* APP_CORE_PROC_H_ */
