#pragma once

// Portable test logic for the dual-USB charger board: two MAX17048 fuel
// gauges (both fixed at 0x36) behind a TS5A23157 analog switch, plus the
// LTC4412 power-path STAT line. See README.md for the schematic mapping.

void battery_test_setup();
void battery_test_loop();
