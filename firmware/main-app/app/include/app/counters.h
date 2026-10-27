/*
 * Reliability counters (ARCHITECTURE.md §6.3). atomic_t, so any thread or
 * ISR can bump them. Snapshotted into COUNTERS event records and shown by
 * "wrist stats".
 */

#ifndef APP_COUNTERS_H_
#define APP_COUNTERS_H_

#include <stdint.h>

enum counter_id {
	CNT_PPG_SLAB_FULL,      /* acq: no free ppg_block, data dropped */
	CNT_PROC_Q_FULL,        /* acq: proc queue full, data dropped */
	CNT_PPG_CHIP_OVF,       /* PPG chip FIFO overflowed before a read */
	CNT_ACCEL_CHIP_OVF,     /* accel chip FIFO overflowed before a drain */
	CNT_I2C_ERR_A,          /* failed ops on bus A */
	CNT_I2C_ERR_B,          /* failed ops on bus B */
	CNT_ANALOG_ERR,         /* failed SAADC reads */
	CNT_BUS_RECOVERIES,
	CNT_POWER_CYCLES,
	CNT_REINITS,
	CNT_REC_DROP_STORAGE,   /* storage queue full */
	CNT_REC_DROP_BLE,       /* ble queue full or notify failed */
	CNT_ZBUS_READ_FAIL,     /* packager could not read a channel in time */
	CNT_PUBLISH_FAIL,       /* processing module could not publish */
	CNT_FLASH_WRITE_ERR,
	CNT_COUNT,
};

/** @brief Increment a counter. Any context (ISR-safe); never blocks. */
void counter_inc(enum counter_id id);

/** @brief Read a counter. Any context; never blocks. */
uint32_t counter_get(enum counter_id id);

/** @brief Counter name for the shell. */
const char *counter_name(enum counter_id id);

/* Sums used by event records (saturated to 16 bits). */
uint16_t counters_i2c_errors(void);
uint16_t counters_ppg_overflows(void);
uint16_t counters_records_dropped(void);
uint16_t counters_recoveries(void);

#endif /* APP_COUNTERS_H_ */
