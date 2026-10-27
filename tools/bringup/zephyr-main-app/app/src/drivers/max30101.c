/*
 * MAX30101 thin driver (implements ppg_drv.h). STUB.
 *
 * Why not the in-tree maxim,max30101 driver: it reads one sample per
 * sample_fetch() (no FIFO burst), routes its interrupt through its own
 * thread/workqueue, and only configures LEDs from devicetree at boot
 * (ARCHITECTURE.md §5).
 *
 * TODO, in order:
 *  init:  i2c_is_ready_dt; read PART_ID (0xFF) == 0x15, else -ENODEV;
 *         MODE_CONFIG.RESET and wait for it to clear; FIFO_CONFIG:
 *         SMP_AVE, FIFO_ROLLOVER_EN = 0, FIFO_A_FULL = 32 - fifo_threshold;
 *         SPO2_CONFIG: ADC range, sample rate, 411 µs pulse width (18-bit);
 *         LED pulse amplitudes; MODE = multi-LED with SLOT1 = green
 *         (SLOT2 = IR if channels == 2); INT_ENABLE1.A_FULL_EN; leave in
 *         SHDN. Configure int-gpios as input with an edge-to-active
 *         interrupt calling cb().
 *  start: clear FIFO_WR_PTR / OVF_COUNTER / FIFO_RD_PTR; read INT_STATUS1
 *         (clears it); MODE_CONFIG.SHDN = 0; enable the GPIO interrupt.
 *  stop:  disable the GPIO interrupt FIRST, then SHDN = 1.
 *  read:  OVF_COUNTER (!= 0 -> overflow); samples = (WR_PTR - RD_PTR) & 31;
 *         ONE burst read of samples x channels x 3 bytes from FIFO_DATA;
 *         unpack 18-bit values; read INT_STATUS1 to release the INT line.
 *  reset: MODE_CONFIG.RESET then re-apply the init config.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>

#include "app/drv/ppg_drv.h"

#define PPG_NODE DT_NODELABEL(ppg)

/* Register map (datasheet table 1). */
#define MAX30101_REG_INT_STATUS1  0x00
#define MAX30101_REG_INT_ENABLE1  0x02
#define MAX30101_REG_FIFO_WR_PTR  0x04
#define MAX30101_REG_OVF_COUNTER  0x05
#define MAX30101_REG_FIFO_RD_PTR  0x06
#define MAX30101_REG_FIFO_DATA    0x07
#define MAX30101_REG_FIFO_CONFIG  0x08
#define MAX30101_REG_MODE_CONFIG  0x09
#define MAX30101_REG_SPO2_CONFIG  0x0A
#define MAX30101_REG_LED1_PA      0x0C
#define MAX30101_REG_LED2_PA      0x0D
#define MAX30101_REG_LED3_PA      0x0E
#define MAX30101_REG_MULTI_LED    0x11
#define MAX30101_REG_PART_ID      0xFF
#define MAX30101_PART_ID          0x15

static const struct i2c_dt_spec i2c = I2C_DT_SPEC_GET(PPG_NODE);
static const struct gpio_dt_spec int_gpio = GPIO_DT_SPEC_GET(PPG_NODE, int_gpios);

static struct ppg_drv_cfg cfg;
static ppg_drv_fifo_cb_t fifo_cb;

int ppg_drv_init(const struct ppg_drv_cfg *c, ppg_drv_fifo_cb_t cb)
{
	cfg = *c;
	fifo_cb = cb;

	if (!i2c_is_ready_dt(&i2c) || !gpio_is_ready_dt(&int_gpio)) {
		return -ENODEV;
	}
	/* TODO: see the file header. */
	return -ENOSYS;
}

int ppg_drv_start(void)
{
	return -ENOSYS;
}

int ppg_drv_stop(void)
{
	/* TODO: gpio_pin_interrupt_configure_dt(&int_gpio, GPIO_INT_DISABLE) first. */
	return -ENOSYS;
}

int ppg_drv_fifo_read(uint32_t samples[][PPG_MAX_CHANNELS], uint8_t max, uint8_t *n,
		      bool *overflow)
{
	ARG_UNUSED(samples);
	ARG_UNUSED(max);
	*n = 0;
	*overflow = false;
	return -ENOSYS;
}

int ppg_drv_reset(void)
{
	return -ENOSYS;
}
