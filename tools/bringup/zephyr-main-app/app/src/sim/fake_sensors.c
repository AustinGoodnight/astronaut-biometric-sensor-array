/*
 * Fake sensors (CONFIG_APP_FAKE_SENSORS): implement every
 * include/app/drv/<name>.h interface with generators, so everything above the
 * driver layer (acq, health, recovery, processing, packager, storage, BLE)
 * runs exactly as it would on the real board.
 *
 * Timing is real: the fake PPG raises its "FIFO almost full" callback from
 * a k_timer at the same period the MAX30101 would, and the FIFO contents
 * are computed from elapsed uptime, so overflows happen if acq falls behind.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>

#include "app/drv/accel_drv.h"
#include "app/drv/analog_drv.h"
#include "app/drv/fuel_gauge_drv.h"
#include "app/drv/ppg_drv.h"
#include "app/drv/temp_drv.h"
#include "app/sim/fake_sensors.h"

LOG_MODULE_REGISTER(fake, CONFIG_APP_LOG_LEVEL);

#define TWO_PI 6.2831853f

#define PPG_BASELINE    100000.0f
#define PPG_AC          3000.0f
#define PPG_RESP_WANDER 1500.0f
#define PPG_NOISE       60
#define PPG_SATURATED   0x3FFFFu

#define ACCEL_NOISE_MG  20
#define ACCEL_MOTION_MG 500.0f

/* --- fault injection ------------------------------------------------------ */

struct fault_state {
	atomic_t mode;      /* enum fake_fault */
	atomic_t remaining; /* ops until auto-clear; 0 = sticky */
};

static struct fault_state faults[SENSOR_COUNT];
static atomic_t hr_bpm = ATOMIC_INIT(72);
static atomic_t motion_on;

void fake_sensors_set_fault(enum sensor_id s, enum fake_fault f, uint32_t count)
{
	if (s >= SENSOR_COUNT) {
		return;
	}
	atomic_set(&faults[s].remaining, count);
	atomic_set(&faults[s].mode, f);
	LOG_WRN("fault on %s: %d (count %u)", sensor_name(s), f, count);
}

void fake_sensors_set_hr(uint16_t bpm)
{
	atomic_set(&hr_bpm, bpm);
}

void fake_sensors_set_motion(bool on)
{
	atomic_set(&motion_on, on);
}

/* Current fault for one op; counts down auto-clearing faults. */
static enum fake_fault fault_tick(enum sensor_id s)
{
	enum fake_fault f = (enum fake_fault)atomic_get(&faults[s].mode);

	if (f != FAKE_FAULT_NONE && atomic_get(&faults[s].remaining) > 0 &&
	    atomic_dec(&faults[s].remaining) == 1) {
		atomic_set(&faults[s].mode, FAKE_FAULT_NONE);
		LOG_INF("fault on %s cleared", sensor_name(s));
	}
	return f;
}

/* -EIO for a NACK fault, else 0. */
static int bus_op(enum sensor_id s)
{
	return fault_tick(s) == FAKE_FAULT_NACK ? -EIO : 0;
}

int fake_sensors_bus_recover(enum app_bus bus)
{
	LOG_INF("bus %c recovery (fake)", bus == BUS_A ? 'A' : 'B');
	return 0;
}

/* Small deterministic noise source (no heap, no locks: one caller thread). */
static int32_t noise(int32_t amp)
{
	static uint32_t lcg = 12345;

	lcg = lcg * 1103515245u + 12345u;
	return (int32_t)((lcg >> 16) % (2 * amp + 1)) - amp;
}

static float uptime_s(void)
{
	return (float)k_uptime_get() / 1000.0f;
}

/* --- PPG -------------------------------------------------------------------- */

static struct {
	struct ppg_drv_cfg cfg;
	ppg_drv_fifo_cb_t cb;
	struct k_timer timer;
	bool started;
	int64_t start_ms;
	uint64_t consumed; /* samples already read since start */
	float phase;       /* cardiac phase 0..1 */
} ppg;

static void ppg_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	if (ppg.cb != NULL) {
		ppg.cb(); /* same path as the real INT pin */
	}
}

/* Pulse shape over one beat: systolic peak + dicrotic wave. */
static float pulse_shape(float ph)
{
	float sys = expf(-((ph - 0.15f) * (ph - 0.15f)) / (2 * 0.05f * 0.05f));
	float dic = 0.3f * expf(-((ph - 0.45f) * (ph - 0.45f)) / (2 * 0.06f * 0.06f));

	return sys + dic;
}

int ppg_drv_init(const struct ppg_drv_cfg *cfg, ppg_drv_fifo_cb_t cb)
{
	ppg.cfg = *cfg;
	ppg.cb = cb;
	k_timer_init(&ppg.timer, ppg_timer_fn, NULL);
	return bus_op(SENSOR_PPG);
}

int ppg_drv_start(void)
{
	int err = bus_op(SENSOR_PPG);

	if (err) {
		return err;
	}
	uint32_t period_ms = 1000U * ppg.cfg.fifo_threshold / ppg.cfg.rate_hz;

	ppg.start_ms = k_uptime_get();
	ppg.consumed = 0;
	ppg.started = true;
	k_timer_start(&ppg.timer, K_MSEC(period_ms), K_MSEC(period_ms));
	return 0;
}

int ppg_drv_stop(void)
{
	k_timer_stop(&ppg.timer);
	ppg.started = false;
	return 0;
}

int ppg_drv_fifo_read(uint32_t samples[][PPG_MAX_CHANNELS], uint8_t max, uint8_t *n,
		      bool *overflow)
{
	enum fake_fault f = fault_tick(SENSOR_PPG);

	*n = 0;
	*overflow = false;
	if (f == FAKE_FAULT_NACK) {
		return -EIO;
	}
	if (!ppg.started) {
		return 0;
	}

	uint64_t produced = (uint64_t)(k_uptime_get() - ppg.start_ms) * ppg.cfg.rate_hz / 1000;
	uint64_t waiting = produced - ppg.consumed;

	if (waiting > PPG_BLOCK_MAX_SAMPLES) {
		/* Chip FIFO overflow: the oldest samples are lost. */
		*overflow = true;
		ppg.consumed = produced - PPG_BLOCK_MAX_SAMPLES;
		waiting = PPG_BLOCK_MAX_SAMPLES;
	}

	uint8_t count = (uint8_t)MIN(waiting, (uint64_t)max);
	float dt = 1.0f / (float)ppg.cfg.rate_hz;
	float beats_per_s = (float)atomic_get(&hr_bpm) / 60.0f;

	for (uint8_t i = 0; i < count; i++) {
		float t = (float)(ppg.consumed + i) * dt;
		uint32_t v;

		ppg.phase += beats_per_s * dt;
		ppg.phase -= floorf(ppg.phase);

		switch (f) {
		case FAKE_FAULT_FLATLINE:
			v = (uint32_t)PPG_BASELINE;
			break;
		case FAKE_FAULT_RANGE:
			v = PPG_SATURATED;
			break;
		default:
			v = (uint32_t)(PPG_BASELINE + PPG_AC * pulse_shape(ppg.phase) +
				       PPG_RESP_WANDER * sinf(TWO_PI * 0.25f * t) +
				       (float)noise(PPG_NOISE));
			break;
		}
		for (int ch = 0; ch < ppg.cfg.channels && ch < PPG_MAX_CHANNELS; ch++) {
			samples[i][ch] = v;
		}
	}
	ppg.consumed += count;
	*n = count;
	return 0;
}

int ppg_drv_reset(void)
{
	(void)ppg_drv_stop();
	return bus_op(SENSOR_PPG);
}

/* --- accelerometer -------------------------------------------------------- */

static struct {
	struct accel_drv_cfg cfg;
	bool started;
	int64_t start_ms;
	uint64_t consumed;
} acc;

#define FAKE_ACCEL_FIFO 32

int accel_drv_init(const struct accel_drv_cfg *cfg, accel_drv_wm_cb_t cb)
{
	ARG_UNUSED(cb); /* the breakout has no accel INT; drained in the PPG pass */
	acc.cfg = *cfg;
	return bus_op(SENSOR_ACCEL);
}

int accel_drv_start(void)
{
	int err = bus_op(SENSOR_ACCEL);

	if (err == 0) {
		acc.start_ms = k_uptime_get();
		acc.consumed = 0;
		acc.started = true;
	}
	return err;
}

int accel_drv_stop(void)
{
	acc.started = false;
	return 0;
}

int accel_drv_fifo_drain(int16_t samples[][3], uint8_t max, uint8_t *n, bool *overflow)
{
	enum fake_fault f = fault_tick(SENSOR_ACCEL);

	*n = 0;
	*overflow = false;
	if (f == FAKE_FAULT_NACK) {
		return -EIO;
	}
	if (!acc.started) {
		return 0;
	}

	uint64_t produced = (uint64_t)(k_uptime_get() - acc.start_ms) * acc.cfg.rate_hz / 1000;
	uint64_t waiting = produced - acc.consumed;

	if (waiting > FAKE_ACCEL_FIFO) {
		*overflow = true;
		acc.consumed = produced - FAKE_ACCEL_FIFO;
		waiting = FAKE_ACCEL_FIFO;
	}

	uint8_t count = (uint8_t)MIN(waiting, (uint64_t)max);
	bool moving = atomic_get(&motion_on) != 0;

	for (uint8_t i = 0; i < count; i++) {
		float t = (float)(acc.consumed + i) / (float)acc.cfg.rate_hz;
		float m = moving ? ACCEL_MOTION_MG * sinf(TWO_PI * 2.0f * t) : 0.0f;

		if (f == FAKE_FAULT_FLATLINE) {
			samples[i][0] = 0;
			samples[i][1] = 0;
			samples[i][2] = 1000;
		} else if (f == FAKE_FAULT_RANGE) {
			samples[i][0] = INT16_MAX;
			samples[i][1] = INT16_MAX;
			samples[i][2] = INT16_MAX;
		} else {
			samples[i][0] = (int16_t)(m + (float)noise(ACCEL_NOISE_MG));
			samples[i][1] = (int16_t)(0.5f * m + (float)noise(ACCEL_NOISE_MG));
			samples[i][2] = (int16_t)(1000 + noise(ACCEL_NOISE_MG));
		}
	}
	acc.consumed += count;
	*n = count;
	return 0;
}

int accel_drv_reset(void)
{
	(void)accel_drv_stop();
	return bus_op(SENSOR_ACCEL);
}

/* --- skin temperature ------------------------------------------------------- */

static bool temp_have_conversion;

int temp_drv_init(void)
{
	temp_have_conversion = false;
	return bus_op(SENSOR_SKIN_TEMP);
}

int temp_drv_read(int32_t *mdegc)
{
	enum fake_fault f = fault_tick(SENSOR_SKIN_TEMP);

	if (f == FAKE_FAULT_NACK) {
		return -EIO;
	}
	/* Mirror the real one-shot pattern: the first read has nothing yet. */
	if (!temp_have_conversion) {
		temp_have_conversion = true;
		return -EAGAIN;
	}
	switch (f) {
	case FAKE_FAULT_FLATLINE:
		*mdegc = 33000;
		break;
	case FAKE_FAULT_RANGE:
		*mdegc = 85000;
		break;
	default:
		*mdegc = 33000 + (int32_t)(500.0f * sinf(TWO_PI * uptime_s() / 600.0f)) + noise(20);
		break;
	}
	return 0;
}

int temp_drv_reset(void)
{
	return temp_drv_init();
}

/* --- fuel gauge -------------------------------------------------------------- */

#define FAKE_BATT_START_PCT 100
#define FAKE_BATT_FLOOR_PCT 15 /* stay above the critical level: no surprise power-off */
#define FAKE_BATT_S_PER_PCT 600

int fg_drv_init(uint8_t alert_pct, fg_drv_alert_cb_t cb)
{
	ARG_UNUSED(alert_pct);
	ARG_UNUSED(cb);
	return bus_op(SENSOR_FUEL_GAUGE);
}

int fg_drv_read(struct fg_reading *out)
{
	enum fake_fault f = fault_tick(SENSOR_FUEL_GAUGE);

	if (f == FAKE_FAULT_NACK) {
		return -EIO;
	}

	int32_t pct = FAKE_BATT_START_PCT - (int32_t)(k_uptime_get() / 1000 / FAKE_BATT_S_PER_PCT);

	pct = MAX(pct, FAKE_BATT_FLOOR_PCT);
	if (f == FAKE_FAULT_RANGE) {
		pct = 200;
	}
	out->soc_pct = (uint8_t)pct;
	out->mv = (uint16_t)(3300 + pct * 9);
	return 0;
}

int fg_drv_clear_alert(void)
{
	return bus_op(SENSOR_FUEL_GAUGE);
}

int fg_drv_reset(void)
{
	return bus_op(SENSOR_FUEL_GAUGE);
}

/* --- analog (GSR, flex) ------------------------------------------------------- */

int analog_drv_init(enum analog_ch ch)
{
	return ch < ANALOG_CH_COUNT ? 0 : -EINVAL;
}

int analog_drv_read_mv(enum analog_ch ch, int32_t *mv)
{
	enum sensor_id s = (ch == ANALOG_CH_GSR) ? SENSOR_GSR : SENSOR_FLEX;
	enum fake_fault f = fault_tick(s);
	float t = uptime_s();

	if (f == FAKE_FAULT_NACK) {
		return -EIO;
	}
	if (f == FAKE_FAULT_FLATLINE) {
		*mv = 1000;
		return 0;
	}
	if (f == FAKE_FAULT_RANGE) {
		*mv = 20000; /* converts to a value outside physiological bounds */
		return 0;
	}
	if (ch == ANALOG_CH_GSR) {
		/* Tonic drift + an occasional phasic bump. */
		*mv = 800 + (int32_t)(100.0f * sinf(TWO_PI * t / 120.0f)) + noise(5);
	} else {
		/* Breathing at 15 /min. */
		*mv = 1500 + (int32_t)(200.0f * sinf(TWO_PI * 0.25f * t)) + noise(5);
	}
	return 0;
}

/* --- shell -------------------------------------------------------------------- */

#if defined(CONFIG_SHELL)

static int parse_sensor(const char *name)
{
	for (int s = 0; s < SENSOR_COUNT; s++) {
		if (strcmp(name, sensor_name(s)) == 0) {
			return s;
		}
	}
	return -1;
}

static int cmd_fault(const struct shell *sh, size_t argc, char **argv)
{
	static const char *const modes[] = {"none", "nack", "flatline", "range"};
	int s = parse_sensor(argv[1]);
	int f = -1;

	for (int i = 0; i < (int)ARRAY_SIZE(modes); i++) {
		if (strcmp(argv[2], modes[i]) == 0) {
			f = i;
		}
	}
	if (s < 0 || f < 0) {
		shell_error(sh, "usage: fake fault <ppg|accel|skin_temp|gsr|flex|fuel_gauge> "
				"<none|nack|flatline|range> [count]");
		return -EINVAL;
	}
	fake_sensors_set_fault(s, f, argc > 3 ? (uint32_t)strtoul(argv[3], NULL, 10) : 0);
	return 0;
}

static int cmd_hr(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	fake_sensors_set_hr((uint16_t)strtoul(argv[1], NULL, 10));
	shell_print(sh, "fake HR %s bpm", argv[1]);
	return 0;
}

static int cmd_motion(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(sh);
	ARG_UNUSED(argc);
	fake_sensors_set_motion(strcmp(argv[1], "0") != 0);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(fake_cmds,
	SHELL_CMD_ARG(fault, NULL, "<sensor> <none|nack|flatline|range> [count]", cmd_fault, 3, 1),
	SHELL_CMD_ARG(hr, NULL, "<bpm>", cmd_hr, 2, 0),
	SHELL_CMD_ARG(motion, NULL, "<0|1>", cmd_motion, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fake, &fake_cmds, "Fake sensors / fault injection", NULL);

#endif /* CONFIG_SHELL */
