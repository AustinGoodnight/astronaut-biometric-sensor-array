# Protocol

The one place the packet formats are defined. Code that implements them
belongs in [common/protocol](../common/protocol).

## Links

<!-- Which device talks to which, over what radio. -->

| Link | Radio | From → To |
|---|---|---|
| Wrist → head module | BLE | |
| Head module → base station | LoRa | |

## LoRa radio configuration

<!-- Frequency, spreading factor, bandwidth, coding rate, TX power, sync word. -->

## LoRa packet format

Current draft (moved from the old root README):

![LoRa transmission byte layout](https://github.com/user-attachments/assets/a81f7a35-d33d-4ab9-9ba0-23bf0eae209e)

- Header: the first 14 bits are the sequence number. The last 2 bits are
  stale flags: the first for BIO, the second for ENV.
- Parity is computed with Reed-Solomon.

The FreeRTOS prototype's payload struct is `SensorPayload_t` in
[tools/bringup/freertos-env-sensors/inc/sensorPayload.h](../tools/bringup/freertos-env-sensors/inc/sensorPayload.h).

## BLE record format (wrist → head module)

<!-- The Zephyr prototype's 32-byte record is described in
tools/bringup/zephyr-main-app/ARCHITECTURE.md. Copy the final layout here. -->

## Error detection and correction

## Versioning

<!-- How a receiver tells which format version a packet uses. -->
