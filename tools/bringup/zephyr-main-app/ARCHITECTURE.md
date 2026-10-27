# Wrist Module Firmware — Architecture

Status: **approved; skeleton implemented** (builds clean, not yet run on hardware). Target: nRF52840 (Raytac module
on the custom wrist board). Dev hardware until then: **Seeed XIAO nRF52840
(`xiao_ble/nrf52840`) on our breakout**, the same board family as MonoWatch.
SDK: **nRF Connect SDK v3.4.0** (same as the MonoWatch project), pinned in
[west.yml](west.yml).

Design priorities, in order: **reliability → low power → simplicity.** When
two choices conflict, the earlier priority wins, and the reason is written
down next to the choice.

---

## 1. What the module does

Every 5 seconds the wrist module produces one 32-byte **record** that
summarizes the last 5 s of physiology (HR avg/min/max, respiration rate,
skin temp, estimated core temp, GSR, activity, battery). The module:

1. **logs every record** to local flash (the authoritative copy), and
2. **notifies it over BLE** to the helmet board (nRF52840, BLE central),
   which relays it over LoRa to the base station.

If BLE is down, nothing is lost: the log has it, and a future backfill
command can fetch it by sequence number.

```
 sensors ──ISR──▶ acq ──ppg_block / sample──▶ proc ──zbus (latest value)──▶ packager
 (I2C, SAADC)    (only bus owner)            (per-module      per metric      (every 5 s,
                                               algorithms)                     never waits)
                                                                                   │ record (32 B, by value)
                                                                        ┌──────────┴──────────┐
                                                                        ▼                     ▼
                                                                     storage                 ble
                                                                  (page batches          (notify if
                                                                   → QSPI NOR)            subscribed)
 health (1 s): watches error counters ─▶ requests recovery from acq, drives LEDs
 main/supervisor: init, then power button, mode changes, power-off, image confirm
 task watchdog: every thread above has its own channel
```

---

## 2. Threads and priorities

Zephyr: **lower number = higher priority.** Negative priorities are
*cooperative* (never preempted by other threads); 0+ are *preemptive*. All
our threads are preemptive. Kernel/stack threads with negative priority (the
system workqueue, the BT host RX path) always run before ours. The BLE radio
timing itself is handled by MPSL/SoftDevice Controller in interrupt
context, so none of our priorities can make the radio miss an event.

| Thread | Prio | Stack (start) | Wakes on | Job |
|---|---|---|---|---|
| `acq` | 2 | 2048 | `k_event` bits (see §2.2) | **Only thread that touches I2C and SAADC.** Reads PPG FIFO + drains accel FIFO in the same pass into one `ppg_block`; polls slow sensors on divisors; executes recovery actions requested by `health`. Never blocks on downstream: if the slab or queue is full, it drops and counts. |
| `proc` | 5 | 3072 | `k_msgq` of `acq_msg` (1 s timeout) | Runs the processing modules; each validates and publishes to its zbus channel. Largest stack because filters and the Kalman step use floats (FPU) and local buffers. |
| `packager` | 6 | 1536 | `k_timer` 5 s (`k_timer_status_sync`) | Reads every result channel, builds a record, marks stale/missing fields, puts it (by value, `K_NO_WAIT`) on the storage and BLE queues. |
| `health` | 7 | 1536 | `k_timer` 1 s | Per-sensor health state machine, counters snapshot, LEDs. Decides on recovery; does **not** touch I2C itself (see §2.3). |
| `storage` | 8 | 2048 | `k_msgq` of records (10 s timeout) | Batches records into a 256-byte page buffer; writes full pages; flushes on request. Stack covers the FCB + flash driver call depth. |
| `ble` | 9 | 2048 | `k_msgq` of `ble_msg` (10 s timeout) | Notifies records if connected and subscribed (otherwise drops); processes control writes forwarded from the BT RX context; starts/stops advertising on mode change. |
| `main` (supervisor) | 10 | 2048 | `k_event` (power button, mode request, low battery) | Runs init in order, starts the threads above, then stays alive as the supervisor: power-off sequence, mode switching, MCUboot image confirm. |

Plus Zephyr's own: system workqueue (−1, coop), BT host threads, logging
(debug only, lowest), shell (debug only), idle.

**Why these priorities.** `acq` is highest because it is the only thread
with a hard deadline: the MAX30101 FIFO holds 32 samples (320 ms at 100 sps),
and the FIFO-almost-full interrupt fires with ~70 ms of headroom. Everything
downstream is buffered and can run late without losing data. `packager`
sits above `storage`/`ble` so a record is always built on time even when a
flash erase or BLE burst is in progress. `health` is above `storage`/`ble` so
it keeps running (and keeps LEDs and recovery working) if one of those is
stuck. The supervisor is lowest because nothing it does is time-critical.

**Why bounded queue waits (1–10 s timeouts) instead of `K_FOREVER`.** Every
thread must feed its task-watchdog channel. A thread that blocks forever on
an empty queue (e.g. `ble` in log-only mode) would starve its channel and
reset the board. Each consumer loop waits with a timeout shorter than its
watchdog timeout and feeds on every iteration, data or not. The extra wakeup
every 10 s is negligible next to the 4 PPG wakeups per second.

**Stack sizes** are starting points. `debug.conf` enables
`CONFIG_THREAD_ANALYZER` (shell: `kernel thread stacks`); once the real
algorithms are in, each stack gets trimmed to measured peak + 25 %.

**Thread start.** All threads are `K_THREAD_DEFINE`d with delay
`K_TICKS_FOREVER` (statically allocated, visible in the map file) and started
explicitly by `main` with `k_thread_start()` **after** every module's
`*_init()` succeeds, consumers first (`storage`, `ble`, `proc`, `packager`,
`health`, then `acq` last), so no producer ever runs before its consumer
exists.

### 2.1 ISRs

ISRs (GPIO callbacks and `k_timer` expiry functions) do exactly one thing:
`k_event_post()` a bit, or `k_sem_give()`. No I2C, no logging, no
allocation. All work happens in threads.

### 2.2 `acq` event bits (`include/app/events.h`)

| Bit | Source | Action in `acq` |
|---|---|---|
| `EVT_PPG_FIFO` | MAX30101 INT pin (GPIO ISR) | Read PPG FIFO, drain accel FIFO, build one `ppg_block`. |
| `EVT_ACCEL_WM` | Accel INT pin (only if `CONFIG_APP_ACCEL_INT_ROUTED`) | Drain accel only (used when PPG is off or failed). |
| `EVT_SLOW_TICK` | `k_timer`, 1 s | Bump divisors; temp/battery reads; start analog front-end warm-up (below). |
| `EVT_ANALOG_READY` | one-shot `k_timer` (front-end settle time) | SAADC read of GSR / flex, then switch the front ends off. |
| `EVT_RECOVER` | `health` thread | Run the queued recovery action (bus recover / re-init / power-cycle). |
| `EVT_MODE` | supervisor | Reconfigure sensors for the new mode, or stop them for power-off. |

If the accel INT pin is **not** routed, the accel FIFO is drained only in the
PPG pass, plus on `EVT_SLOW_TICK` if PPG is down. The accel FIFO must then
hold ≥ 1.5 s of data (true for any candidate part at 25 Hz).

**Analog front ends use two phases instead of a sleep.** GSR and flex need
their front end powered for a few ms before the reading is valid. Instead of
`k_sleep()` inside `acq` (which would delay a PPG read), the slow tick turns
the front end on and starts a one-shot timer; `EVT_ANALOG_READY` does the
SAADC conversion and turns the front end off. `acq` stays responsive the whole
time.

### 2.3 Deviations from the original brief (and why)

| # | Brief said | Problem found | What this design does |
|---|---|---|---|
| D1 | `health` triggers bus recovery and re-init | That would make `health` a second thread touching I2C, breaking the "acq is the only bus owner" invariant (which is what lets us skip bus mutexes). | `health` *decides* and writes a request into a 1-slot mailbox, then posts `EVT_RECOVER`; `acq` executes it between reads. |
| D2 | A failure on bus A never stops bus B | One thread drives both buses. The nRF TWIM driver's per-transfer timeout defaults to **500 ms** (`CONFIG_I2C_NRFX_TRANSFER_TIMEOUT`); a stuck bus A would stall the temp/battery reads behind it. | Set the timeout to **20 ms**. `acq` skips any sensor in `FAILED` state except at its slow-retry slot. Worst-case delay to bus B per pass ≈ 2 × 20 ms. If measurements show that is still too much, fall back to one `acq` thread per bus (documented, not built). |
| D3 | Power-cycle via the sensor-rail load switch as the last step | With one shared rail, power-cycling for a bus-A fault would also reset the bus-B sensors. | **Board team agreed: one load switch per I2C bus** (`rail-a-en`, `rail-b-en`). Power-cycle is per bus and only re-inits that bus's sensors. It is still rate-limited (≤ 1 per bus per 10 min) so a dead part can't make the rail flap. |
| D4 | — | Main thread would return after init and waste its stack. | `main` becomes the supervisor thread (power-off, mode changes, image confirm) instead of adding a 7th thread. |
| D5 | Algorithms stay stubs | With pure `-ENOSYS` stubs no metric is ever valid, so the validity-mask and fault-injection path can't be demonstrated end to end. | `*_validate()` is **real** (bounds, rate of change, quality; it's simple and it is the reliability feature). Algorithm stubs publish a clearly marked **placeholder** (e.g. HR = 60.00 bpm when a non-flat `ppg_block` arrives; skin temp/GSR/battery pass the raw converted value). A PPG fault means no blocks, HR goes stale, and the validity bit clears. |
| D6 | Record = 32 B notification | The default ATT MTU is 23 (20-byte payload), so a 32-byte notification doesn't fit. | `CONFIG_BT_L2CAP_TX_MTU=65` + matching ACL buffer sizes; the helmet (our firmware) calls `bt_gatt_exchange_mtu()` on connect. `ble_proto.h` documents the minimum MTU (35). |
| D7 | Event / time-anchor records go to storage | — | All three record types go to **both** storage and BLE (same 32-byte shape, record-type byte distinguishes). The helmet gets fault/boot events live at no extra cost. |

---

## 3. Data flow and contracts

### 3.1 Memory: no heap

`CONFIG_HEAP_MEM_POOL_SIZE=0`. All buffers are static:

| Object | Mechanism | Size | Why |
|---|---|---|---|
| `ppg_block` | `K_MEM_SLAB`, 8 blocks | 8 × ~460 B ≈ 3.7 KB | Big, so pass by pointer; 8 blocks = 2 s of slack for `proc` falling behind. |
| `acq → proc` | `K_MSGQ` of `struct acq_msg` (16 deep) | pointer to block, or a small slow-sample by value | |
| result channels | zbus, one channel per metric | `struct metric_value` (12 B) or `struct hr_summary` | "Latest value" semantics: the packager wants the newest, not a queue. |
| records | `K_MSGQ` × 2 (storage 16 deep, ble 8 deep) | 32 B by value | Small; copying avoids ownership bugs between two consumers. |
| storage page buffer | static `uint8_t[256]` | 8 records | One NOR page program per 40 s. |

**Slab ownership rule:** `acq` allocates; `proc` frees after the last
processing module returns. Nobody else touches a block. If `proc` can't
free (bug), the slab empties, `acq` counts `ppg_overflow`, and `health`
reports PPG DEGRADED. That makes the leak visible instead of silent.

### 3.2 `struct ppg_block` (`include/app/ppg_block.h`)

```c
#define PPG_BLOCK_MAX_SAMPLES   32   /* MAX30101 FIFO depth */
#define PPG_MAX_CHANNELS        2    /* green (+ IR for skin-contact detection) */
#define ACCEL_BLOCK_MAX_SAMPLES 32

struct ppg_block {
	uint32_t t_read_ms;        /* k_uptime when the FIFO read completed */
	uint16_t ppg_rate_hz;      /* nominal, as configured on the chip */
	uint16_t accel_rate_hz;    /* nominal */
	uint8_t  ppg_n;            /* samples per channel in this block */
	uint8_t  ppg_channels;     /* active LED channels */
	uint8_t  accel_n;          /* may be 0 if accel FAILED / not present */
	uint8_t  flags;            /* PPG_BLK_OVF (chip FIFO overflowed), ACCEL_BLK_OVF */
	uint32_t ppg[PPG_BLOCK_MAX_SAMPLES][PPG_MAX_CHANNELS];   /* 18-bit raw ADC */
	int16_t  accel[ACCEL_BLOCK_MAX_SAMPLES][3];              /* raw, mg scale in header */
};
/* NOTE: PPG and accel run on separate chip oscillators that drift by a few %.
 * Sample i's time is reconstructed backwards from t_read_ms at the nominal
 * rate. Processing must RESAMPLE accel onto the PPG timebase (using the
 * actual sample counts per block to estimate the true rate ratio) before
 * using it for motion-artifact reduction. Never assume index alignment. */
```

Defaults: PPG 100 sps, 1 green channel (IR optional), FIFO-almost-full at 24
samples, so 4 interrupts/s with 80 ms of slack. Accel 25 Hz, so ~6 samples
per block. All of these are Kconfig-tunable.

### 3.3 `struct metric_value` (`include/app/metric.h`)

```c
enum metric_status {
	METRIC_OK = 0,
	METRIC_STALE,         /* set by the packager: ts older than the metric's max age */
	METRIC_INVALID,       /* set by *_validate(): out of bounds / rate / low quality */
	METRIC_SENSOR_FAULT,  /* source sensor is FAILED */
	METRIC_NOT_PRESENT,   /* compiled out or sensor absent */
};

struct metric_value {
	int32_t  value;       /* fixed point, scale per metric in units.h */
	uint32_t ts_ms;       /* uptime of the newest input sample that produced it */
	uint8_t  quality;     /* 0..100 */
	uint8_t  status;      /* enum metric_status */
};
```

`units.h` is the **single** place scale factors live (HR in 0.01 bpm,
temperatures in 0.001 °C, GSR in nS, respiration in 0.01 br/min, and so on),
together with the narrower record encodings in §4.

### 3.4 Processing module pattern

Every processing module (`hr`, `motion`, `resp`, `core_temp`,
`skin_temp_proc`, `gsr_proc`) follows the same shape so it can be handed to
one teammate:

```c
int  hr_init(void);                                  /* resets ctx; main, before threads */
int  hr_process_block(const struct ppg_block *blk);  /* proc thread; never blocks */
int  hr_validate(const struct hr_summary *candidate,
                 const struct hr_ctx_view *ctx);     /* pure function: bounds, rate, quality */
void hr_get_beat_intervals(...);                     /* read-only view for resp / core_temp */
```

- **State lives in one file-static context struct** per module: history
  ring buffer, filter state, last valid value + timestamp, health counters.
  Threads hold no state. Multi-second windows (5 s HR, 30–60 s
  respiration) live here.
- **`*_validate()` is separate and pure:** physiological bounds, max
  rate of change versus last *valid* value, and minimum signal quality. An
  invalid result is **published with `METRIC_INVALID`**, never replaced
  with the held value. (A held value that looks fresh is the most dangerous
  bug a physiological monitor can have: a flatlined sensor would read as a
  healthy, steady HR.)
- Only `proc` calls these functions, so contexts need no locks. The
  cross-module read (`resp` and `core_temp` reading HR's beat intervals)
  also happens on the `proc` thread.
- **Floats are allowed inside processing modules** (the nRF52840 has an
  FPU, and Kalman filters are far clearer in float). Everything published
  or transmitted is fixed-point. `CONFIG_FPU_SHARING=y`, because more than
  one thread may touch FP registers (`proc`, plus logging `%f` in debug).

Intended algorithms (documented in each header, stubbed in code):

| Module | Input | Algorithm (to implement) | Output channel |
|---|---|---|---|
| `motion` | accel from `ppg_block` | Magnitude high-pass → activity counts over 5 s; motion flag when above threshold. Runs **before** `hr` in each block. | `chan_activity`, motion flag |
| `hr` | PPG green + resampled accel | Band-pass 0.5–4 Hz → motion-artifact reduction (adaptive filter / spectral subtraction using accel) → peak detection with refractory period → beat intervals → 5 s avg/min/max. Quality lowered by motion flag and IR-proximity (off-wrist). | `chan_hr` (`struct hr_summary`) |
| `resp` | **Kconfig choice:** `APP_RESP_SRC_FLEX` (flex ADC) or `APP_RESP_SRC_PPG` (default) | FLEX: band-pass 0.1–0.7 Hz, zero-crossing/peak count over 30 s. PPG: fuse amplitude (RIAV), baseline (RIIV) and beat-interval (RIFV) modulation. | `chan_resp` |
| `core_temp` | HR series (1 per 5 s) | ECTemp-style 1-D Kalman filter: CT_k = CT_{k−1} + w; HR = f(CT) + v with a quadratic f; state persists for hours; runs at the record cadence. | `chan_core_temp` |
| `skin_temp_proc` | skin temp samples | Median-of-3 + EMA; bounds 20–42 °C. | `chan_skin_temp` |
| `gsr_proc` | GSR ADC | ADC to conductance, EMA; bounds; flag electrode-off (open circuit). | `chan_gsr` |
| battery | fuel gauge SOC | Pass-through from `acq` (no processing). | `chan_battery` |

### 3.5 zbus channels (`include/app/channels.h`)

Metric channels: `chan_hr`, `chan_resp`, `chan_skin_temp`, `chan_core_temp`,
`chan_gsr`, `chan_activity`, `chan_battery`. System channels: `chan_health`
(per-sensor states, published by `health`), `chan_mode`, `chan_link` (helmet
link/battery info from BLE control writes).

The packager reads metric channels with `zbus_chan_read(..., K_MSEC(5))`.
It never waits longer. If the read fails, it marks the field missing and
counts it. `mode`/`health` changes are delivered to `ble`, `storage` and the
LED logic through zbus **listeners** that only do a non-blocking queue put or
event post. Nothing heavy runs in publisher context.

---

## 4. Record format (`include/app/record.h`)

All records are **exactly 32 bytes, packed, little-endian** (the nRF52840 is
LE; the base station decodes with Python `struct` `'<'`). A fixed size
makes the log a flat array (seq → offset math), one notification per record,
and one decoder.

### 4.1 Common header (12 B) and trailer (2 B)

| Off | Size | Field | Notes |
|---|---|---|---|
| 0 | 1 | `record_type` | `0x01` DATA, `0x02` TIME_ANCHOR, `0x03` EVENT |
| 1 | 1 | `format_version` | starts at 1; bump on any layout change |
| 2 | 2 | `boot_count` | persisted in settings, incremented at every boot |
| 4 | 4 | `seq` | monotonic per boot, shared by all record types; `(boot_count, seq)` is globally unique and ordered, which is what backfill uses |
| 8 | 4 | `uptime_ms` | `k_uptime_get_32()` at build time (wraps at 49.7 days) |
| 30 | 2 | `crc16` | CRC-16/CCITT-FALSE (`crc16_itu_t`, seed 0xFFFF) over bytes 0–29 |

### 4.2 DATA record payload (bytes 12–29, 18 B)

| Off | Size | Field | Encoding |
|---|---|---|---|
| 12 | 2 | `validity_mask` | bit = 1 → field valid **and** fresh. b0 hr_avg, b1 hr_min, b2 hr_max, b3 resp, b4 skin_temp, b5 core_temp, b6 gsr, b7 activity, b8 battery, b9–15 reserved (0) |
| 14 | 2 | `health` | 2 bits per sensor: `00` OK, `01` DEGRADED, `10` FAILED, `11` NOT_PRESENT/OFF. Slots: 0 ppg, 1 accel, 2 skin_temp, 3 gsr, 4 flex, 5 fuel_gauge, 6 storage, 7 reserved |
| 16 | 2 | `hr_avg` | uint16, 0.1 bpm |
| 18 | 1 | `hr_min` | uint8, 1 bpm |
| 19 | 1 | `hr_max` | uint8, 1 bpm |
| 20 | 1 | `resp_rate` | uint8, 0.5 breaths/min (0–127.5) |
| 21 | 1 | `activity` | uint8, 0–255 activity index (scale in `units.h`) |
| 22 | 2 | `skin_temp` | int16, 0.01 °C |
| 24 | 2 | `core_temp` | int16, 0.01 °C (estimated) |
| 26 | 2 | `gsr` | uint16, 0.01 µS |
| 28 | 1 | `battery` | uint8, % |
| 29 | 1 | `flags` | b0 motion, b1 off_wrist, b2 charging, b3 low_battery, b4 time_synced (anchor exists this boot), b5 data_dropped since last record, b6–7 reserved |

An invalid field is written as **0 with its validity bit cleared**. The value
is never held over. The per-field *reason* (stale vs. invalid vs. fault) is
recoverable from `health` + `flags`, and the full reason goes in the
event log.

### 4.3 TIME_ANCHOR payload (written when the helmet sends wall-clock time)

| Off | Size | Field |
|---|---|---|
| 12 | 8 | `epoch_ms` (uint64, Unix ms from the helmet) |
| 20 | 1 | `source` (0 helmet RTC, 1 helmet GPS, 2 base station, …) |
| 21 | 9 | reserved (0) |

The header's `uptime_ms` is the local time at receipt, so
`(uptime_ms, epoch_ms)` pairs let the base station map every record in that
boot to wall-clock time, with drift correction if there are two or more anchors.

### 4.4 EVENT payload

| Off | Size | Field |
|---|---|---|
| 12 | 1 | `event_code`: BOOT, POWER_OFF, MODE_CHANGE, SENSOR_FAULT, SENSOR_RECOVERED, LOW_BATTERY, COUNTERS (periodic snapshot), STORAGE_WRAP, CMD_REJECTED |
| 13 | 1 | `arg8` (e.g. sensor id, new mode) |
| 14 | 2 | `arg16` (e.g. new health state, error code) |
| 16 | 4 | `arg32` (e.g. `hwinfo` reset-cause bits for BOOT) |
| 20 | 2 | `i2c_errors` (saturating) |
| 22 | 2 | `ppg_overflows` |
| 24 | 2 | `records_dropped` |
| 26 | 2 | `recoveries` |
| 28 | 2 | reserved |

A COUNTERS event is emitted every 10 min and on power-off, so long-term
reliability stats are in the log even without a debug shell.

Layout is enforced with `BUILD_ASSERT(sizeof(...) == 32)` and
`offsetof` asserts on every field.

---

## 5. Sensors and drivers

Two layers:

- **`src/drivers/`**: register-level, no policy. Each implements a small
  interface in `include/app/drv/*.h` (e.g. `ppg_drv_fifo_read()`). **The
  fake-sensor generators implement these same interfaces**, so with
  `CONFIG_APP_FAKE_SENSORS=y` everything above the driver (power sequencing,
  health, recovery requests, processing) runs exactly as on real hardware.
- **`src/sensors/`**: app-facing wrappers: power enables (boost, front
  ends, bus rails), unit conversion, error reporting to `health`, DT lookups.
  Called only from `acq`.

| Part | In-tree NCS 3.4.0 driver? | Choice | Why |
|---|---|---|---|
| MAX30101 PPG | Yes (`maxim,max30101`) but **inadequate**: `sample_fetch` reads one sample per call (no FIFO burst read), triggers go through the driver's own thread/workqueue, LED and mode settings are DT-only at boot. | **Own thin driver** `drivers/max30101.c` (compatible `abs,max30101`, so the in-tree driver isn't auto-enabled). | We need one burst read of the FIFO, our own GPIO ISR posting `EVT_PPG_FIFO`, and runtime LED current/shutdown control for power. |
| Accelerometer (TBD) | Many (LIS2DW12, LIS2DH, BMI270, …), but FIFO burst support varies by driver and API (classic vs. RTIO stream). | **`accel_drv.h` interface** + `drivers/accel_<part>.c` per candidate later; only `fake` and `none` now. | Part is undecided. The interface (configure ODR/range/FIFO watermark, `fifo_drain(buf, max)`, optional wake-on-motion) is what the rest of the code sees. |
| MAX30205 / MAX30208 | **Neither** is in tree. | **Own driver** `drivers/max3020x.c` behind `temp_drv.h`, Kconfig choice selects the part. | MAX30205: one-shot mode (shutdown + OS bit) for low power. MAX30208: has its own FIFO + GPIO; same interface, different internals. |
| MAX17048 fuel gauge | Yes (`fuel_gauge` API), but **no ALRT configuration**. | **Own thin driver** `drivers/max17048.c` behind `fuel_gauge_drv.h`. | We need ALRT threshold/clear and raw register access for diagnostics (the previous charger board's gauges never ACKed, so being able to dump registers is worth the ~150 lines). |
| GSR, flex | Zephyr ADC API + in-tree SAADC driver: **adequate.** | `analog_drv.h` (real: ADC; fake: generator). | — |
| QSPI NOR | In-tree `nordic,qspi-nor`: **adequate** (has deep power-down, runtime PM). | Used by the NOR storage backend. | — |

**Board-level signals** (all in the board overlay, pins `TODO` until the
schematic is final; the XIAO overlay uses the bring-up map in
[README.md](README.md#xiao-breakout-pin-map-proposed)):

- I2C bus A = `i2c0` (TWIM0): PPG `0x57`, accel. Bus B = `i2c1` (TWIM1):
  skin temp `0x48`, fuel gauge `0x36`. Both use
  `zephyr,pm-device-runtime-auto`.
- Interrupt inputs: `ppg-int-gpios`, `accel-int-gpios` (optional),
  `fg-alert-gpios`. These go on the sensor nodes (custom bindings in
  `app/dts/bindings/`).
- Enables in the `zephyr,user` node: `rail-a-en-gpios` (PPG + accel),
  `rail-b-en-gpios` (skin temp + fuel gauge),
  `boost-5v-en-gpios`, `gsr-en-gpios`, `flex-en-gpios`, `power-hold-gpios`.
- Status LEDs and power button: standard `leds` / `gpio-keys` nodes; button
  long-hold via the in-tree `zephyr,input-longpress`.

**Power sequencing rules** (enforced in `src/sensors/`): disable a sensor's
GPIO interrupt **before** cutting its power, otherwise a floating or
pulled-low INT line fires spurious interrupts. Re-enable it only after
re-init. Put I2C pins into the pinctrl sleep state (disconnected) while the
bus rail is off, otherwise the sensors get back-powered through the pull-ups.

---

## 6. Reliability

### 6.1 Task watchdog

`task_wdt` with `CONFIG_TASK_WDT_HW_FALLBACK=y` (nRF WDT, keeps running in
sleep). One channel per thread:

| Channel | Timeout | Fed |
|---|---|---|
| acq | 3 s | every loop (slow tick guarantees ≥ 1 Hz) |
| proc | 5 s | every loop (1 s queue timeout) |
| packager | 12 s | every 5 s record |
| health | 5 s | every 1 s tick |
| storage | 30 s | every loop (10 s timeout; covers a worst-case 64 KB erase ≈ 3.5 s) |
| ble | 30 s | every loop (10 s timeout) |
| supervisor | 30 s | every loop (10 s timeout) |

A watchdog reset is recorded in the next BOOT event (`hwinfo` reset cause).
If it happens before the new image is confirmed, MCUboot reverts (§10).

### 6.2 Per-sensor health state machine (`health.c`, real)

Sensors: ppg, accel, skin_temp, gsr, flex, fuel_gauge (+ storage reported
in the same table). `acq`/`sensors` report every op result with
`health_report(sensor, err)`, which is lock-free (atomic counters) and can
be called from `acq`.

```
            err ×3 consecutive              escalation exhausted
   OK ─────────────────────▶ DEGRADED ───────────────────────────▶ FAILED
    ▲                           │                                    │
    └──── success ×5 ───────────┘◀──── retry succeeds ───────────────┘
                                                        (slow retry every 60 s)
```

Escalation while DEGRADED, one step per health tick (≥ 1 s apart),
**executed by `acq`** (D1):

1. **retry**: just let the next scheduled read happen.
2. **bus recovery**: `i2c_recover_bus()` (9 clock pulses + STOP) on that
   sensor's bus.
3. **sensor re-init**: soft reset + reconfigure.
4. **power-cycle** that sensor's bus rail (rate-limited, D3), then re-init
   every sensor on that bus. The other bus is untouched.
5. **FAILED**: `acq` skips it; a slow retry (re-init) runs every 60 s.

Isolation: escalation is per sensor; bus recovery touches only that bus; a
FAILED sensor costs other sensors nothing (D2); the packager and storage
never depend on any sensor. State changes emit SENSOR_FAULT /
SENSOR_RECOVERED event records and publish `chan_health`.

### 6.3 Counters

`include/app/counters.h`: `atomic_t` counters for `ppg_overflow` (slab/queue
full), `chip_fifo_overflow`, `i2c_err[bus]`, `bus_recoveries`,
`power_cycles`, `records_dropped_storage`, `records_dropped_ble`,
`zbus_read_fail`, `flash_write_err`. Exposed in COUNTERS event records and,
in debug builds, via the shell: `wrist stats`, `wrist health`,
`fake fault …`.

### 6.4 Persistence and boot

Settings subsystem on NVS in the internal `storage_partition` (32 KB):
`app/boot_cnt`, `app/mode`, `app/reset_hist` (last 4 reset causes). Boot:
read and increment `boot_cnt`, save it **before** starting threads, read
`hwinfo_get_reset_cause()` then clear it, emit the BOOT event as the first
record of the boot.

---

## 7. Low power

| Mechanism | How |
|---|---|
| Tickless idle | Default in Zephyr; no periodic tick. The CPU sleeps unless an event bit, timer or BLE event is pending. |
| Peripheral runtime PM | `CONFIG_PM_DEVICE=y`, `CONFIG_PM_DEVICE_RUNTIME=y`; `zephyr,pm-device-runtime-auto` on `i2c0`, `i2c1`, `adc`, `qspi`. TWIM/SAADC/QSPI suspend between transfers. The QSPI NOR enters deep power-down (≈ 0 µA) between page writes. |
| DC/DC | NCS 3.4 sets it in devicetree: `&reg1 { regulator-initial-mode = <NRF5X_REG_MODE_DCDC>; };` (already set in the XIAO board files). **TODO (Raytac board):** enable it only if the Raytac module has the DC/DC inductor. Enabling DC/DC without the inductor stops the chip from running. |
| 32 kHz clock | `CONFIG_CLOCK_CONTROL_NRF_K32SRC_XTAL=y` (LFXO; the XIAO has the crystal; MonoWatch BLE runs on it). **TODO (Raytac board):** if the module has no crystal, use `debug`/board conf with `CONFIG_CLOCK_CONTROL_NRF_K32SRC_RC=y` (+ calibration, BLE sleep-clock accuracy 500 ppm). |
| No polling | All sensor data is interrupt- or timer-driven. The analog front-end settle uses a one-shot timer (§2.2). |
| Switched front ends | 5 V boost only while PPG runs; GSR and flex front ends on only for their ADC read (~10 ms per second). |
| BLE | Peripheral preferred connection parameters: interval 500 ms, peripheral latency 8, supervision timeout 12 s. One notification per 5 s. 0 dBm TX. Advertising interval 1 s. Off entirely in LOG_ONLY. |
| Logging | Debug: log + shell over USB CDC ACM (the XIAO has no debugger, so no RTT). Release: `CONFIG_LOG=n`, no console, no shell. |

### 7.1 Power estimate (assumptions to verify on the bench)

| Consumer | Assumption | Avg current |
|---|---|---|
| nRF52840 idle (System ON, RTC, RAM retained) | datasheet | 3 µA |
| CPU active (DC/DC, 64 MHz, ~3.3 mA) | ~3 % duty (4 PPG passes/s + processing) | 100 µA |
| BLE connection | 500 ms interval, latency 8, 1 notify / 5 s | 30 µA |
| MAX30101 core (1.8 V) | 100 sps, 1 LED | 600 µA |
| Green LED via 5 V boost | 10 mA pulse × 215 µs × 100 Hz = 215 µA at 5 V; ÷ 0.8 efficiency × 5/3.7 V | 360 µA |
| Boost converter quiescent | while PPG on | 30 µA |
| Accelerometer | 25 Hz low-power mode + FIFO | 15 µA |
| Skin temp (MAX30205 one-shot) | 1 conversion / 10 s | 5 µA |
| GSR front end (op-amp ~1 mA) | on 10 ms/s | 10 µA |
| Flex divider (~300 µA) | on 10 ms/s | 3 µA |
| MAX17048 | active mode | 25 µA |
| QSPI NOR | DPD; 1 page program / 40 s; 4 KB erase / ~11 min | 5 µA |
| Status LEDs | 2 ms flash every 2 s at 2 mA | 2 µA |
| Regulators, leakage | board-dependent | 50 µA |
| **Total (NORMAL)** | | **≈ 1.25 mA** |
| **Total (LOG_ONLY)** | minus BLE | **≈ 1.2 mA** |

At 250 mAh that is ~200 h, far beyond the 5 h / 20 h requirements. **Power
is not the binding constraint, so reliability wins every trade-off** (for
example, PPG at 100 sps instead of 50, and IR on for off-wrist detection).
Bench checks needed: PPG LED setting (the dominant term after the MAX30101
core), and BLE with the helmet's real connection parameters.

**Peak current (< 30 mA):** the LED pulse current comes out of the boost
output capacitor, so its battery-side peak depends on the boost converter's
inductor current limit (**board ask:** choose a boost with a limit of
≤ 25 mA, or a large enough output cap). BLE TX ≈ 5 mA, NOR erase ≈ 4 mA,
CPU ≈ 3.3 mA. Firmware rule: never erase flash while the boost is starting
up (the storage thread checks a flag; cheap insurance).

---

## 8. BLE

- Wrist = peripheral, helmet = central. One custom 128-bit service
  (UUIDs in `ble_proto.h`):

| Char | Props | Payload |
|---|---|---|
| `data` | notify | one 32-byte record (any type) |
| `status` | read, notify | `struct wrist_status` (12 B): proto version, mode, battery %, health (16 bits), flags, last command result, uptime s. Notified on change, at most once per 5 s. |
| `control` | write (with response) | `[opcode u8][payload]` |

| Opcode | Name | Payload | Effect |
|---|---|---|---|
| `0x01` | TIME_SYNC | `u64 epoch_ms`, `u8 source` | Emits a TIME_ANCHOR record. |
| `0x02` | HELMET_STATUS | `u8 lora_link`, `u8 helmet_batt_pct`, `u8 flags` | Publishes `chan_link`; drives the link LED. |
| `0x03` | SET_MODE | `u8 mode` | Mode request to the supervisor (same path as the button). |
| `0x04` | BACKFILL_REQ | `u16 boot`, `u32 seq_from`, `u32 seq_to` | **Reserved; replies with ATT error `0x80` (not supported).** |
| `0x05`–`0x7F` | reserved | | ATT error `0x80`. |

Malformed lengths get ATT error `0x0D` (invalid attribute length). Writes
arrive in the BT RX context: the GATT callback only validates the length and
copies the write into the `ble` thread's queue (non-blocking); the work
happens in `ble`. `ble_proto.h` contains only fixed-width types, packed
structs and `#define`s (no Zephyr includes), so the helmet firmware can
include it unchanged.

- Security: phase 1 (bring-up) has none, so nRF Connect for Mobile can
  subscribe. Phase 2 (proposed): LESC Just Works + bonding, then advertise
  only to the bonded helmet (filter accept list). This ties into open
  decision 5 (how pairing mode is entered).
- MTU: needs ≥ 35 (D6).
- BLE SMP for firmware update (`CONFIG_APP_BLE_SMP`, default n) adds the
  SMP service to the same connection.

---

## 9. Storage

Interface (`include/app/storage.h`): `storage_init()`,
`storage_submit(const struct record *)` (non-blocking queue put, callable
from any thread), `storage_flush(k_timeout_t)` (blocks the **caller** until
the page is written; supervisor only), `storage_read_by_seq(boot, seq, out, n)`
(stub, for backfill and USB readout), `storage_stats()`.

Backend chosen by Kconfig choice `APP_STORAGE_BACKEND`:

- **`APP_STORAGE_BACKEND_NOR`** (default): Zephyr **FCB** (Flash Circular
  Buffer) on a `log_partition` on the external QSPI NOR (the XIAO's onboard
  **P25Q16H, 2 MB**; the Raytac board's chip and size are TBD). **One FCB entry =
  one page batch of 8 records (256 B).** FCB sectors are 64 KB logical
  sectors (32 on 2 MB, so the sector table is 256 B of RAM). 2 MB holds about
  65 000 records, ≈ 3.8 days of continuous logging, which covers several
  20 h missions. When the log is full, FCB rotates and erases the oldest
  64 KB (≈ 2.8 h of the oldest records). (`MODE_RAW_WAVEFORM` would need
  ≈ 1 MB/h, so it needs a bigger chip or an SD card.)
  - **Why FCB over raw sectors:** power loss mid-write is the case that
    matters. FCB writes the entry, then its CRC, and on mount it discards a
    torn entry and resumes at the last good one. Rotation, wrap and mount
    recovery are in-tree and tested. A hand-rolled raw log would have to
    reimplement exactly that, and it would be the riskiest code in the
    project. Each record also carries its own CRC16, so a corrupted record
    inside a valid entry is still caught.
  - **What a power loss can cost:** the unwritten RAM page (≤ 8 records =
    40 s). Mitigations: flush on mode change, power-off, LOW_BATTERY
    event, and before any reset the firmware initiates.
  - In the **skeleton**, the NOR backend is a stub that logs "would write
    page N" with record count and seqs. Wiring real FCB on the XIAO's P25Q16H
    is a small, self-contained next task.
- **`APP_STORAGE_BACKEND_SD`**: stub (SPIM3 + FATFS or raw blocks; same
  interface).

---

## 10. Modes, power button, firmware update

### 10.1 Modes (`mode.c`)

`MODE_NORMAL` (BLE + log), `MODE_LOG_ONLY` (BLE disabled), and
`MODE_RAW_WAVEFORM` (stretch, stub: would log PPG blocks). The mode is
persisted in settings and published on `chan_mode`. Changes come from
`mode_request(new, source)` via two paths: a button pattern (stub; the UX is
an open decision) and BLE SET_MODE. The supervisor applies them in order:
storage flush, MODE_CHANGE event, publish, then each module reacts
(`ble` stops or starts advertising, `acq` reconfigures sensors).

### 10.2 Power-off (`power.c`, run by the supervisor)

Long hold (3 s, `zephyr,input-longpress`) or a critical battery level
(≤ 2 %) triggers this sequence:

1. Publish `MODE_OFF`: `acq` stops sensors (INT disabled, boost off, front
   ends off, both bus rails off); the packager stops.
2. Emit POWER_OFF and COUNTERS event records.
3. `storage_flush(K_SECONDS(2))`.
4. Release `power-hold` (the push-button controller cuts power).
5. Wait 500 ms. If the board is still alive (USB-powered or the hold
   circuit failed), enter nRF **System OFF** (`sys_poweroff()`) with the
   button configured as the wake source.

### 10.3 Firmware update (MCUboot from day one)

The XIAO ships with the **Adafruit nRF52 UF2 bootloader** (double-tap reset,
then drag a `.uf2` onto the USB drive). Installing MCUboot **replaces** that
bootloader, and the only way to do it is over SWD. The XIAO has no onboard
debugger, so that needs an external probe. That gives two boot setups for
the same application:

| | **XIAO, now** (no probe) | **MCUboot**: Raytac board, and the XIAO once we have a probe |
|---|---|---|
| Bootloader | factory UF2 bootloader | MCUboot (sysbuild) |
| Update | double-tap reset, drag `zephyr.uf2` | mcumgr SMP over USB CDC ACM; BLE SMP optional |
| Partitions | board default (`nrf52840_partition_uf2_sdv7.dtsi`): app 0x27000 (788 KB), settings 0xEC000 (32 KB), UF2 bootloader 0xF4000 | table below |
| Build switch | `SB_CONFIG_BOOTLOADER_MCUBOOT=n`, `CONFIG_BUILD_OUTPUT_UF2=y` | `SB_CONFIG_BOOTLOADER_MCUBOOT=y` |
| Rollback on a bad image | none (re-drag the old `.uf2`) | swap with revert (below) |

"From day one" means the MCUboot variant is configured now and must **build
warning-free** alongside the UF2 variant, so it can't rot. It just can't be
*run* until there's a probe. **Recommendation: buy an SWD probe now**
(Raspberry Pi Debug Probe ≈ $12, or J-Link EDU Mini ≈ $60). The blank Raytac
module needs one for its very first flash regardless, and with it we can test
MCUboot, rollback and SMP on the XIAO before the board arrives.

MCUboot partition layout (devicetree; Partition Manager is deprecated in NCS 3.4):

| Partition | Flash | Offset | Size |
|---|---|---|---|
| `boot_partition` (MCUboot) | internal | 0x00000 | 48 KB |
| `slot0_partition` (app) | internal | 0x0C000 | 472 KB |
| `slot1_partition` (update) | internal | 0x82000 | 472 KB |
| `storage_partition` (settings/NVS) | internal | 0xF8000 | 32 KB |
| `log_partition` (FCB) | QSPI NOR | 0x0 | whole chip |

- **Secondary slot stays internal.** Putting it on QSPI would require
  MCUboot itself to carry the QSPI driver (a larger bootloader and one more
  thing that can break the boot path). The app, BLE included, is expected
  to fit well under 472 KB. If it doesn't, moving slot1 to the NOR is a
  devicetree-only change.
- **USB DFU:** mcumgr SMP over USB CDC ACM (`USB_DEVICE_STACK_NEXT`, as in
  the NCS `smp_svr` sample's `cdc.conf`) in the **application**. Debug builds
  use two CDC ACM ports (console/shell + SMP), release builds one (SMP). USB is
  only active when VBUS is present, so it costs nothing on battery. If the app
  is broken, recover with SWD.
- **BLE SMP** behind `CONFIG_APP_BLE_SMP`.
- Upgrade mode is **swap with revert**: the new image runs as a test image.
  The supervisor calls `boot_write_img_confirmed()` only after 60 s with no
  watchdog reset and storage + BLE up. A crash or hang before that boots the
  old image. (Compiled only in the MCUboot variant.)
- Signing key: MCUboot's default dev key for now. **TODO:** generate a
  project key and keep it **out of the repo**.

---|---|---|---|
| `boot_partition` (MCUboot) | internal | 0x00000 | 48 KB |
| `slot0_partition` (app) | internal | 0x0C000 | 472 KB |
| `slot1_partition` (update) | internal | 0x82000 | 472 KB |
| `storage_partition` (settings/NVS) | internal | 0xF8000 | 32 KB |
| `log_partition` (FCB) | QSPI NOR | 0x0 | 8 MB |

- **Secondary slot stays internal for now.** Putting it on QSPI would require
  MCUboot itself to carry the QSPI driver (a larger bootloader and one more
  thing that can break the boot path). The app, BLE included, is expected
  to fit well under 472 KB. If it doesn't, moving slot1 to the NOR is a
  devicetree-only change.
- **USB DFU:** mcumgr SMP over USB CDC ACM (`USB_DEVICE_STACK_NEXT`, as in
  the NCS `smp_svr` sample's `cdc.conf`) in the **application**, uploaded
  with `mcumgr`/`nrfutil`/nRF Connect Device Manager. USB is only active
  when VBUS is present, so it costs nothing on battery. If the app is broken,
  recover with SWD.
- **BLE SMP** behind `CONFIG_APP_BLE_SMP`.
- Upgrade mode is **swap with revert**: the new image runs as a test image.
  The supervisor calls `boot_write_img_confirmed()` only after 60 s with no
  watchdog reset and storage + BLE up. A crash or hang before that boots the
  old image.
- Signing key: MCUboot's default dev key for now. **TODO:** generate a
  project key and keep it **out of the repo**.

---

## 11. Fake sensors (`CONFIG_APP_FAKE_SENSORS`)

`src/sim/fake_sensors.c` implements every `include/app/drv/*.h` interface:

- PPG: a synthetic waveform (systolic peak + dicrotic notch template) at a
  settable HR (default 72 bpm), with baseline wander at the respiration rate
  and noise. A `k_timer` at the FIFO-almost-full period posts `EVT_PPG_FIFO`,
  so the `acq` path is identical to the real interrupt path.
- Accel: low noise plus optional motion bursts. Temp: slow drift around
  33 °C. GSR: slow drift + phasic bumps. Battery: linear discharge.
- **Fault injection** (shell, debug builds): `fake fault <sensor>
  <none|nack|flatline|range> [count]`. `nack` makes every driver op on the
  sensor return `-EIO`, which drives the health state machine through the
  full escalation. With a `count`, the fault clears by itself after that many
  ops, so recovery can be shown; without one it is sticky, so FAILED can be
  shown. `flatline` and `range` make the data bad, which exercises
  `*_validate()` and the validity mask. Also `fake hr <bpm>` and
  `fake motion <0|1>`. Step-by-step test: README "Testing on the XIAO".

**Acceptance test on the XIAO:** inject `nack` on skin_temp. Expected: the
skin_temp health slot goes DEGRADED then FAILED, validity bit b4 clears,
HR/GSR/etc. stay valid, records keep coming every 5.0 s, and SENSOR_FAULT
event records appear in the log and over BLE.

---

## 12. Project layout and build

```
tools/bringup/zephyr-main-app/
  west.yml                 NCS v3.4.0 pin
  ARCHITECTURE.md  CLAUDE.md  README.md
  app/
    CMakeLists.txt  Kconfig  prj.conf  debug.conf  release.conf
    boards/xiao_ble_nrf52840.overlay     (+ Raytac wrist board later: boards/<vendor>/wrist/)
    dts/bindings/          abs,max30101  abs,max3020x  abs,max17048  abs,accel  vendor-prefixes.txt
    mcuboot/               sysbuild.conf  app.conf  partitions.overlay   (MCUboot variant)
    sysbuild/              mcuboot.conf  mcuboot.overlay                (bootloader image)
    include/app/           events.h record.h metric.h units.h channels.h ppg_block.h counters.h ble_proto.h
      core/                acq.h proc.h packager.h health.h mode.h power.h supervisor.h
                           watchdog.h leds.h persist.h
      drv/                 ppg_drv.h accel_drv.h temp_drv.h fuel_gauge_drv.h analog_drv.h
      sensors/             sensors.h (all wrappers)  board.h (rails, boost, enables, bus recovery)
      processing/          hr.h motion.h resp.h core_temp.h skin_temp_proc.h gsr_proc.h validate.h
      comms/               ble.h
      storage/             storage.h storage_backend.h
      sim/                 fake_sensors.h
    src/
      main.c
      core/                acq proc packager health mode power supervisor channels record
                           counters persist watchdog leds shell
      drivers/             max30101 max3020x max17048 analog_saadc accel_none   (stubs)
      sensors/             board ppg accel skin_temp gsr flex fuel_gauge
      processing/          validate hr motion resp core_temp skin_temp_proc gsr_proc
      comms/               ble ble_svc
      storage/             storage storage_nor (stub) storage_sd (stub)
      sim/                 fake_sensors
```

**Real vs stub.** Real: every thread and its wiring, the task watchdog
(with starved-channel attribution across the reset), the health state
machine and recovery ladder, record building/CRC/emission, the packager's
deadline loop and validity mask, all `*_validate()` functions, zbus
channels, page batching and flush in storage, the BLE service, mode
switching and the power-off sequence, settings persistence, and the fake
sensors. Stubs: register-level drivers (`-ENOSYS`, with the register map and
the steps in a TODO), the NOR/SD backends (the NOR one logs what it would
write), and the processing algorithms (placeholder outputs, D5).

The custom board is added later as a board definition in the app's
`BOARD_ROOT` (`-b wrist/nrf52840`). Code only references devicetree node
labels and `zephyr,user` properties, never pin numbers, so adding the
board needs no source changes.

Build commands, including the workaround for the space in the repo path:
README "Build and flash" and CLAUDE.md "Build".

Main init order (`main.c`). **Nothing in init is fatal:** a settings,
storage or sensor failure is logged and shows up in health and the
records, and the board keeps running. A degraded module that still
reports beats a reboot loop.

1. settings: read and clear the reset cause, boot_count++, save
2. read the starved-watchdog-channel marker from the previous boot
3. record module (seq = 0, this boot's count)
4. task watchdog init (HW fallback)
5. health init (all OK), LEDs
6. storage init (backend mount; result → storage health), mode load
7. board power (both bus rails on, boost and front ends off) and every
   sensor init (result → health: OK / FAILED / NOT_PRESENT)
8. processing module inits, packager
9. power button
10. BLE init (`bt_enable`; advertising starts in the ble thread, NORMAL only)
11. emit BOOT event record (reset cause, starved channel)
12. start threads, consumers first: storage, ble, proc, packager, health, acq
13. main becomes the supervisor loop (image confirm after 60 s healthy, MCUboot variant only)

---

## 13. Open decisions (kept swappable, not decided here)

| # | Decision | How the code keeps it swappable |
|---|---|---|
| 1 | Accelerometer part; INT pin routed or not | `accel_drv.h`; `CONFIG_APP_ACCEL_PART_*` choice; `CONFIG_APP_ACCEL_INT_ROUTED` |
| 2 | MAX30205 vs MAX30208 | `temp_drv.h`; `CONFIG_APP_SKIN_TEMP_MAX30205` / `_MAX30208` |
| 3 | Respiration source: flex vs PPG-derived | `CONFIG_APP_RESP_SRC_FLEX` / `CONFIG_APP_RESP_SRC_PPG` (default) choice; `resp.h` defines both inputs |
| 4 | Storage backend: NOR vs SD | `storage_backend.h` ops table; `CONFIG_APP_STORAGE_BACKEND_*` |
| 5 | Mode-selection UX (button pattern vs BLE vs both); pairing entry | `mode_request(mode, source)` is the single entry; both paths stubbed |
| 6 | NCS version | **Proposed: v3.4.0** (matches MonoWatch, installed at `/opt/nordic/ncs/v3.4.0`). Pinned in `west.yml`; confirm. |
| 7 | Raytac module: DC/DC inductor? 32 kHz crystal? | Devicetree `&reg1` mode + K32SRC Kconfig, both marked TODO in the board files |

### Hardware requirements for the board team

Moved to [README.md → Hardware requirements](README.md#hardware-requirements-what-the-firmware-assumes),
the single list the board design follows (1.8 V LDO behind bus A's switch,
per-bus load switches, pull-up placement, boost peak current, SWD, test
points, XIAO breakout pin map).
