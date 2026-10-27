# tools

Development and test support. Nothing here ships as part of the system;
the shipping firmware lives in [devices/](../devices).

| Folder | What it is | MCU |
|---|---|---|
| [test-base-station](test-base-station) | Bench LoRa TX/RX pair (Wio-SX1262) that stands in for the base station, plus the BLE/GPS range-test web app in `range-app/` | XIAO ESP32-C6 |
| [bringup/zephyr-main-app](bringup/zephyr-main-app) | Zephyr / NCS skeleton of the biometric-wrist firmware | nRF52840 |
| [bringup/freertos-env-sensors](bringup/freertos-env-sensors) | FreeRTOS prototype: SCD30 CO₂ + fast analog sensor packed into a LoRa payload | XIAO ESP32-C6 |
| [bringup/hr-max30102-ble](bringup/hr-max30102-ble) | MAX30102 heart-rate bring-up streamed over BLE, plus the PPG dashboard | XIAO ESP32-C6 |
| [bringup/hr-max30102-wifi](bringup/hr-max30102-wifi) | The same MAX30102 bring-up streamed over WiFi (TCP) | XIAO ESP32-C6 |
| [bringup/temp-max30205](bringup/temp-max30205) | MAX30205 body-temperature bring-up | XIAO ESP32-C6 |
| [bringup/battery-sense](bringup/battery-sense) | Tests for the old dual-USB charger board's MAX17048 fuel gauges | nRF52820 / XIAO ESP32-C6 |
| [bringup/lora-esp32c3](bringup/lora-esp32c3) | First LoRa TX/RX test (SX127x) and its USB config UI | XIAO ESP32-C3 |
| [flash-scripts](flash-scripts) | Flashing scripts (empty) | — |

## Testing web UIs

The HR dashboard (`bringup/hr-max30102-ble/ppg_dashboard.html`) and the range
app (`test-base-station/range-app/`) talk to a single node directly over BLE
or serial and run on a phone or laptop, with no LoRa link needed. They are
separate from the base-station web UI in
[devices/base-station/web-ui](../devices/base-station/web-ui). Both are published to GitHub Pages by
[.github/workflows/pages.yml](../.github/workflows/pages.yml) (at
`/hr-testing/` and `/lora-range/`).
