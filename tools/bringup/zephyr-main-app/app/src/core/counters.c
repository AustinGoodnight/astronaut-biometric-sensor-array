#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "app/counters.h"

static atomic_t counters[CNT_COUNT];

static const char *const names[CNT_COUNT] = {
	[CNT_PPG_SLAB_FULL] = "ppg_slab_full",
	[CNT_PROC_Q_FULL] = "proc_q_full",
	[CNT_PPG_CHIP_OVF] = "ppg_chip_ovf",
	[CNT_ACCEL_CHIP_OVF] = "accel_chip_ovf",
	[CNT_I2C_ERR_A] = "i2c_err_a",
	[CNT_I2C_ERR_B] = "i2c_err_b",
	[CNT_ANALOG_ERR] = "analog_err",
	[CNT_BUS_RECOVERIES] = "bus_recoveries",
	[CNT_POWER_CYCLES] = "power_cycles",
	[CNT_REINITS] = "reinits",
	[CNT_REC_DROP_STORAGE] = "rec_drop_storage",
	[CNT_REC_DROP_BLE] = "rec_drop_ble",
	[CNT_ZBUS_READ_FAIL] = "zbus_read_fail",
	[CNT_PUBLISH_FAIL] = "publish_fail",
	[CNT_FLASH_WRITE_ERR] = "flash_write_err",
};

void counter_inc(enum counter_id id)
{
	if (id < CNT_COUNT) {
		atomic_inc(&counters[id]);
	}
}

uint32_t counter_get(enum counter_id id)
{
	return id < CNT_COUNT ? (uint32_t)atomic_get(&counters[id]) : 0;
}

const char *counter_name(enum counter_id id)
{
	return id < CNT_COUNT ? names[id] : "?";
}

static uint16_t sat16(uint32_t v)
{
	return (uint16_t)MIN(v, UINT16_MAX);
}

uint16_t counters_i2c_errors(void)
{
	return sat16(counter_get(CNT_I2C_ERR_A) + counter_get(CNT_I2C_ERR_B));
}

uint16_t counters_ppg_overflows(void)
{
	return sat16(counter_get(CNT_PPG_SLAB_FULL) + counter_get(CNT_PROC_Q_FULL) +
		     counter_get(CNT_PPG_CHIP_OVF));
}

uint16_t counters_records_dropped(void)
{
	return sat16(counter_get(CNT_REC_DROP_STORAGE) + counter_get(CNT_REC_DROP_BLE));
}

uint16_t counters_recoveries(void)
{
	return sat16(counter_get(CNT_BUS_RECOVERIES) + counter_get(CNT_POWER_CYCLES) +
		     counter_get(CNT_REINITS));
}
