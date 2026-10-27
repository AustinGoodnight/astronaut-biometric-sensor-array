#pragma once

// Thin hardware layer so battery_test.cpp builds unchanged on both targets:
//   hal_zephyr.cpp  - nRF52820 (Zephyr, pins from the devicetree overlay)
//   hal_arduino.cpp - ESP32-C6 (Arduino, pins #defined at the top)
// Only one of them is compiled, picked by __ZEPHYR__ / ARDUINO.

#include <stddef.h>
#include <stdint.h>

bool hal_init();

// Returns true if a device ACKs at addr.
bool hal_i2c_probe(uint8_t addr);
bool hal_i2c_write(uint8_t addr, const uint8_t *buf, size_t len);
// Write wlen bytes, repeated start, read rlen bytes.
bool hal_i2c_write_read(uint8_t addr, const uint8_t *wbuf, size_t wlen,
                        uint8_t *rbuf, size_t rlen);

// SELECT net -> TS5A23157 IN1/IN2. Low = NC side, high = NO side.
void hal_set_select(bool high);
// STATUS net <- LTC4412 STAT (470k pull-up to VOUT).
bool hal_read_status();

void hal_delay_ms(uint32_t ms);
uint32_t hal_millis();
// printf-style; integer formats only (no %f) so the Zephyr side doesn't need
// float printf support.
void hal_printf(const char *fmt, ...);
