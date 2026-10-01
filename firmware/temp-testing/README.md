# temp-testing (MAX30205 bring-up)

PlatformIO project for bringing up a MAX30205 body-temperature sensor on a
Seeed XIAO ESP32C6 over I2C and streaming readings over USB serial. Setup
(VS Code + pioarduino, drivers, ports) is identical to
[firmware/hr-testing](../hr-testing/CLAUDE.md).

## Wiring

| MAX30205 | XIAO ESP32C6 |
|---|---|
| VDD | 3V3 |
| GND | GND |
| SDA | D4 (GPIO22) |
| SCL | D5 (GPIO23) |

The I2C address comes from the A0–A2 straps (0x48 with all to GND). The
firmware scans 0x40–0x5F, so you don't need to change code for a
different strap setting.

## Build/upload/monitor

```
pio run -e xiao_c6 -t upload
pio device monitor -e xiao_c6
```

## Output

On boot the firmware prints an I2C bus scan, then checks the THYST/TOS
registers against the chip's power-on defaults (0x4B00 / 0x5000). The
MAX30205 has no ID register, so this is the only way to identify it. After
that it prints one CSV line per second:

```
temp_c,temp_f
32.125,89.825
```

Resolution is 1/256 °C. If a read fails (for example, the sensor is
unplugged), it goes back to probing once a second.
