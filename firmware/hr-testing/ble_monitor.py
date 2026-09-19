#!/usr/bin/env python3
"""Wireless log monitor for the HR-Testing board's Nordic UART Service.

Scans for the "HR-Testing" BLE peripheral, connects, subscribes to the NUS
TX characteristic, and prints every notified log line to stdout - the
wireless equivalent of `pio device monitor`. Reconnects automatically if the
board resets or drops the link.

Usage:
    python3 ble_monitor.py
    pip install -r requirements.txt   # first time, if bleak isn't installed
"""
import asyncio
import sys
from datetime import datetime

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "HR-Testing"
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX_CHAR_UUID = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"


def on_notify(_sender, data: bytearray) -> None:
    timestamp = datetime.now().strftime("%H:%M:%S")
    print(f"[{timestamp}] {data.decode(errors='replace')}")


async def find_device():
    return await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name == DEVICE_NAME
        or NUS_SERVICE_UUID in [u.lower() for u in adv.service_uuids],
        timeout=10.0,
    )


async def run() -> None:
    while True:
        print(f"Scanning for {DEVICE_NAME}...")
        device = await find_device()
        if device is None:
            print("Not found, retrying...")
            continue

        print(f"Connecting to {device.address}...")
        try:
            async with BleakClient(device) as client:
                print("Connected. Waiting for log lines (Ctrl+C to quit)...")
                await client.start_notify(NUS_TX_CHAR_UUID, on_notify)
                while client.is_connected:
                    await asyncio.sleep(1)
        except Exception as exc:
            print(f"Connection error: {exc}")

        print("Disconnected, rescanning...")


def main() -> None:
    try:
        asyncio.run(run())
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
