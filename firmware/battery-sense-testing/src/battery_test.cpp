#include "battery_test.h"

#include "hal.h"

// MAX17048 register map (datasheet table 2). All registers are 16-bit,
// MSB first.
#define GAUGE_ADDR   0x36
#define REG_VCELL    0x02
#define REG_SOC      0x04
#define REG_VERSION  0x08
#define REG_CONFIG   0x0C
#define REG_VALRT    0x14
#define REG_CRATE    0x16
#define REG_VRESETID 0x18
#define REG_STATUS   0x1A

// STATUS high-byte flags.
#define STATUS_RI 0x0100 // reset indicator: gauge just powered up
#define STATUS_VH 0x0200 // VCELL above VALRT.MAX
#define STATUS_VL 0x0400 // VCELL below VALRT.MIN
#define STATUS_VR 0x0800 // voltage reset
#define STATUS_HD 0x1000 // SOC < 1% (empty)
#define STATUS_SC 0x2000 // SOC changed by 1%

#define VALRT_DEFAULT 0x00FF

const uint32_t SAMPLE_INTERVAL_MS = 1000;
const uint32_t SWITCH_SETTLE_MS = 2; // TS5A23157 switches in ns; this is generous
const int HEADER_EVERY_N_ROWS = 30;

// Power-path sanity thresholds (see checkPowerPath()).
const int32_t PATH_BAT1_MARGIN_MV = 50;
const int32_t PATH_BAT2_MARGIN_MV = 400;

// Which battery each switch position reaches. From the schematic:
//   SELECT low  -> NC1/NC2 -> SCL1/SDA1 -> U9, VDD = BAT2
//   SELECT high -> NO1/NO2 -> SCL2/SDA2 -> U4, VDD = BAT1
struct Channel {
  const char *name;
  bool select;
};
const Channel CHANNELS[] = {
    {"BAT1", true},
    {"BAT2", false},
};
const int NUM_CHANNELS = sizeof(CHANNELS) / sizeof(CHANNELS[0]);

struct Reading {
  bool ok;
  uint32_t vcell_uv;
  uint16_t soc_centi;   // 0.01 %
  int32_t crate_centi;  // 0.01 %/hr, positive = charging
  uint16_t status;
};

void selectChannel(const Channel &ch) {
  hal_set_select(ch.select);
  hal_delay_ms(SWITCH_SETTLE_MS);
}

bool readReg(uint8_t reg, uint16_t &out) {
  uint8_t buf[2];
  if (!hal_i2c_write_read(GAUGE_ADDR, &reg, 1, buf, 2)) return false;
  out = ((uint16_t)buf[0] << 8) | buf[1];
  return true;
}

bool writeReg(uint8_t reg, uint16_t value) {
  uint8_t buf[3] = {reg, (uint8_t)(value >> 8), (uint8_t)(value & 0xFF)};
  return hal_i2c_write(GAUGE_ADDR, buf, 3);
}

// Prints an integer in hundredths as "[-]X.YY".
void printCenti(int32_t v) {
  if (v < 0) {
    hal_printf("-");
    v = -v;
  }
  hal_printf("%ld.%02ld", (long)(v / 100), (long)(v % 100));
}

// Prints microvolts as volts with mV resolution, "X.YYY".
void printVolts(uint32_t uv) {
  uint32_t mv = (uv + 500) / 1000;
  hal_printf("%lu.%03lu", (unsigned long)(mv / 1000), (unsigned long)(mv % 1000));
}

// Compact flag string for the CSV, e.g. "RI|VL" or "-".
void printFlags(uint16_t status) {
  static const struct {
    uint16_t bit;
    const char *name;
  } FLAGS[] = {
      {STATUS_RI, "RI"}, {STATUS_VH, "VH"}, {STATUS_VL, "VL"},
      {STATUS_VR, "VR"}, {STATUS_HD, "HD"}, {STATUS_SC, "SC"},
  };
  bool any = false;
  for (const auto &f : FLAGS) {
    if (!(status & f.bit)) continue;
    hal_printf(any ? "|%s" : "%s", f.name);
    any = true;
  }
  if (!any) hal_printf("-");
}

const char *supplyName(bool status) {
  // LTC4412 STAT is open-drain and sinks 10uA once SENSE rises ~20mV above
  // VIN, i.e. when BAT2 (through CR2) is carrying the load. With R7 = 470k
  // that pulls STATUS low. Otherwise STATUS floats high and BAT1 is feeding
  // the load through the AO3401A.
  return status ? "BAT1" : "BAT2";
}

bool readGauge(const Channel &ch, Reading &r) {
  selectChannel(ch);
  uint16_t vcell, soc, crate, status;
  r.ok = readReg(REG_VCELL, vcell) && readReg(REG_SOC, soc) &&
         readReg(REG_CRATE, crate) && readReg(REG_STATUS, status);
  if (!r.ok) return false;

  r.vcell_uv = (uint32_t)vcell * 625 / 8;               // 78.125 uV/LSB
  r.soc_centi = (uint16_t)((uint32_t)soc * 100 / 256);  // 1/256 %/LSB
  r.crate_centi = (int32_t)(int16_t)crate * 208 / 10;   // 0.208 %/hr/LSB
  r.status = status;
  return true;
}

// RI means the gauge just came out of POR, so its SOC is a fresh estimate
// from the first voltage reading. Note it and clear it so a second reset
// later in the run is visible.
void handleReset(const Channel &ch, uint16_t status) {
  if (!(status & STATUS_RI)) return;
  hal_printf("# %s gauge reported a reset (RI); SOC is re-estimating from VCELL\n",
             ch.name);
  selectChannel(ch);
  writeReg(REG_STATUS, status & ~STATUS_RI);
}

// ---------------------------------------------------------------------------
// Boot self-test
// ---------------------------------------------------------------------------

void scanChannel(const Channel &ch) {
  selectChannel(ch);
  hal_printf("#   I2C scan via %s side:", ch.name);
  int found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    if (!hal_i2c_probe(addr)) continue;
    hal_printf(" 0x%02X", addr);
    found++;
  }
  hal_printf(found ? "\n" : " nothing\n");
}

bool identifyGauge(const Channel &ch) {
  selectChannel(ch);
  uint16_t version, config, vresetId, status;
  if (!readReg(REG_VERSION, version) || !readReg(REG_CONFIG, config) ||
      !readReg(REG_VRESETID, vresetId) || !readReg(REG_STATUS, status)) {
    hal_printf("#   %s: no response at 0x%02X - FAIL\n", ch.name, GAUGE_ADDR);
    return false;
  }

  // MAX17048/49 report 0x001X. Anything else is a different part or a bus
  // problem.
  bool versionOk = (version & 0xFFF0) == 0x0010;
  hal_printf("#   %s: VERSION=0x%04X CONFIG=0x%04X ID=0x%02X STATUS=0x%04X -> %s\n",
             ch.name, version, config, vresetId & 0xFF, status,
             versionOk ? "MAX17048 OK" : "unexpected VERSION - FAIL");
  handleReset(ch, status);
  return versionOk;
}

// Both gauges sit at 0x36, so a stuck or miswired switch would make one gauge
// answer for both. Write a different tag into each gauge's VALRT register,
// then read both back: if they match each other, we're talking to the same
// chip twice. VALRT only drives the (unconnected) ALRT pin, and the tags keep
// MAX at 5.1 V and MIN at 20/40 mV so no alert fires; it's restored after.
bool testSwitchIsolation() {
  const uint16_t TAGS[NUM_CHANNELS] = {0x01FF, 0x02FF};
  bool ok = true;

  for (int i = 0; i < NUM_CHANNELS; i++) {
    selectChannel(CHANNELS[i]);
    if (!writeReg(REG_VALRT, TAGS[i])) ok = false;
  }

  for (int i = 0; i < NUM_CHANNELS; i++) {
    selectChannel(CHANNELS[i]);
    uint16_t got = 0;
    bool read = readReg(REG_VALRT, got);
    bool match = read && got == TAGS[i];
    hal_printf("#   %s: wrote VALRT=0x%04X, read back 0x%04X -> %s\n",
               CHANNELS[i].name, TAGS[i], got, match ? "OK" : "FAIL");
    if (!match) ok = false;
  }

  for (int i = 0; i < NUM_CHANNELS; i++) {
    selectChannel(CHANNELS[i]);
    writeReg(REG_VALRT, VALRT_DEFAULT);
  }

  if (!ok) {
    hal_printf("#   Isolation FAIL: SELECT may not be reaching IN1/IN2, or the\n"
               "#   switch is unpowered (VOUT) / one gauge is absent.\n");
  }
  return ok;
}

bool selfTest() {
  hal_printf("#\n# === Battery sense self-test ===\n");
  hal_printf("# Power path: STATUS=%s -> load supplied by %s\n",
             hal_read_status() ? "HIGH" : "LOW", supplyName(hal_read_status()));

  hal_printf("# Bus scan (expect only 0x%02X on each side):\n", GAUGE_ADDR);
  for (const auto &ch : CHANNELS) scanChannel(ch);

  hal_printf("# Gauge identity:\n");
  bool ok = true;
  for (const auto &ch : CHANNELS) ok = identifyGauge(ch) && ok;

  hal_printf("# Switch isolation:\n");
  ok = testSwitchIsolation() && ok;

  hal_printf("# Self-test %s\n#\n", ok ? "PASSED" : "FAILED (continuing anyway)");
  if (!ok) {
    hal_printf("# If nothing answers on either side: the switch and gauges run\n"
               "# off the board's own rails, so at least one battery must be\n"
               "# connected, and SCL/SDA need 4.7k pull-ups to 3V3 (see\n"
               "# README).\n#\n");
  }
  return ok;
}

// ---------------------------------------------------------------------------
// Periodic CSV
// ---------------------------------------------------------------------------

// The load is fed by BAT1 through the ideal diode, or by BAT2 through the
// CR2 Schottky (~0.2-0.3 V drop). So BAT2 should only take over once it is
// well above BAT1. Flag readings that contradict that.
const char *checkPowerPath(bool status, const Reading &b1, const Reading &b2) {
  if (!b1.ok || !b2.ok) return "n/a";
  int32_t b1mv = b1.vcell_uv / 1000;
  int32_t b2mv = b2.vcell_uv / 1000;
  if (!status && b1mv > b2mv + PATH_BAT1_MARGIN_MV) return "BAT2_BUT_BAT1_HIGHER";
  if (status && b2mv > b1mv + PATH_BAT2_MARGIN_MV) return "BAT1_BUT_BAT2_HIGHER";
  return "ok";
}

void printHeader() {
  hal_printf("t_s,bat1_v,bat1_soc,bat1_crate,bat1_flags,"
             "bat2_v,bat2_soc,bat2_crate,bat2_flags,supply,path_check\n");
}

void printReadingFields(const Reading &r) {
  if (!r.ok) {
    hal_printf("ERR,ERR,ERR,ERR,");
    return;
  }
  printVolts(r.vcell_uv);
  hal_printf(",");
  printCenti(r.soc_centi);
  hal_printf(",");
  printCenti(r.crate_centi);
  hal_printf(",");
  printFlags(r.status);
  hal_printf(",");
}

void battery_test_setup() {
  if (!hal_init()) {
    hal_printf("# HAL init failed (I2C/GPIO device not ready). Halting.\n");
    while (true) hal_delay_ms(1000);
  }
  selfTest();
}

void battery_test_loop() {
  static uint32_t lastSample = 0;
  static int rowsSinceHeader = HEADER_EVERY_N_ROWS;
  static bool wasOk[NUM_CHANNELS] = {true, true};

  uint32_t now = hal_millis();
  if (now - lastSample < SAMPLE_INTERVAL_MS) {
    hal_delay_ms(10);
    return;
  }
  lastSample = now;

  Reading r[NUM_CHANNELS];
  for (int i = 0; i < NUM_CHANNELS; i++) {
    readGauge(CHANNELS[i], r[i]);
    if (r[i].ok) handleReset(CHANNELS[i], r[i].status);
    if (r[i].ok != wasOk[i]) {
      hal_printf("# %s gauge %s\n", CHANNELS[i].name,
                 r[i].ok ? "responding again" : "stopped responding");
      wasOk[i] = r[i].ok;
    }
  }
  bool status = hal_read_status();

  if (rowsSinceHeader >= HEADER_EVERY_N_ROWS) {
    printHeader();
    rowsSinceHeader = 0;
  }
  rowsSinceHeader++;

  hal_printf("%lu,", (unsigned long)(now / 1000));
  printReadingFields(r[0]);
  printReadingFields(r[1]);
  hal_printf("%s,%s\n", supplyName(status), checkPowerPath(status, r[0], r[1]));
}
