#!/usr/bin/env python3
"""High-rate raw sample receiver for the HR-Testing board's binary streaming
characteristic (distinct from the text log in ble_monitor.py).

Connects, subscribes to the raw-sample characteristic, unpacks each batch
(a 2-byte sequence number + RAW_BATCH_SIZE (ir, red) uint32 pairs, all
little-endian - must match the RawSample/RAW_BATCH_SIZE layout in
src/main_filtered.cpp), tracks dropped batches via the sequence number, and
prints throughput stats once a second. Pass --csv <path> to also write every
sample to a CSV file for offline analysis.

macOS's CoreBluetooth doesn't let an app request/force the BLE connection
interval - the peripheral firmware asks for a fast one, but the OS decides
per-connection whether to grant it, with no visibility into why. A slow
grant makes this raw stream (which notifies far more often than once per
interval) go silent, even though everything is otherwise healthy. Rather
than requiring a manual reconnect, this script detects that (no packet
within CONNECT_TIMEOUT_S of subscribing) and automatically disconnects and
retries until it lands a connection with a fast interval.

Usage:
    python3 ble_raw_stream.py
    python3 ble_raw_stream.py --csv samples.csv
"""
import argparse
import asyncio
import csv
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

DEVICE_NAME = "HR-Testing"
NUS_SERVICE_UUID = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RAW_CHAR_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"

RAW_BATCH_SIZE = 24  # must match RAW_BATCH_SIZE in src/main_filtered.cpp
PACKET_FORMAT = "<H" + "II" * RAW_BATCH_SIZE
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)

# A healthy (fast-interval) connection delivers the first batch well under a
# second; anything slower means this connection got stuck with a slow
# interval grant - not worth waiting out, just reconnect and try for a
# better one.
CONNECT_TIMEOUT_S = 3.0


class Stats:
    def __init__(self):
        self.samples = 0
        self.batches = 0
        self.dropped_batches = 0
        self.last_seq = None
        self.window_start = time.monotonic()

    def record(self, seq: int):
        self.batches += 1
        self.samples += RAW_BATCH_SIZE
        if self.last_seq is not None:
            gap = (seq - self.last_seq) & 0xFFFF
            if gap > 1:
                self.dropped_batches += gap - 1
        self.last_seq = seq

    def maybe_report(self):
        now = time.monotonic()
        elapsed = now - self.window_start
        if elapsed < 1.0:
            return
        rate = self.samples / elapsed
        print(
            f"{rate:6.1f} samples/s  "
            f"({self.batches} batches, {self.dropped_batches} dropped this window)"
        )
        self.samples = 0
        self.batches = 0
        self.dropped_batches = 0
        self.window_start = now


async def find_device():
    return await BleakScanner.find_device_by_filter(
        lambda d, adv: d.name == DEVICE_NAME
        or NUS_SERVICE_UUID in [u.lower() for u in adv.service_uuids],
        timeout=10.0,
    )


async def run(csv_path: str | None) -> None:
    stats = Stats()
    csv_writer = None
    csv_file = None
    if csv_path:
        csv_file = open(csv_path, "w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["seq", "sample_index", "ir", "red"])

    got_first_packet = asyncio.Event()

    def on_notify(_sender, data: bytearray) -> None:
        got_first_packet.set()
        if len(data) != PACKET_SIZE:
            print(f"unexpected packet size {len(data)} (expected {PACKET_SIZE}), skipping")
            return
        fields = struct.unpack(PACKET_FORMAT, data)
        seq = fields[0]
        samples = fields[1:]
        stats.record(seq)
        if csv_writer:
            for i in range(RAW_BATCH_SIZE):
                ir, red = samples[2 * i], samples[2 * i + 1]
                csv_writer.writerow([seq, i, ir, red])
        stats.maybe_report()

    try:
        while True:
            print(f"Scanning for {DEVICE_NAME}...")
            device = await find_device()
            if device is None:
                print("Not found, retrying...")
                continue

            print(f"Connecting to {device.address}...")
            try:
                async with BleakClient(device) as client:
                    got_first_packet.clear()
                    await client.start_notify(RAW_CHAR_UUID, on_notify)
                    try:
                        await asyncio.wait_for(got_first_packet.wait(), CONNECT_TIMEOUT_S)
                    except asyncio.TimeoutError:
                        print(
                            f"No data within {CONNECT_TIMEOUT_S:.0f}s - likely a slow "
                            "connection-interval grant from macOS, reconnecting..."
                        )
                        continue

                    print("Streaming raw samples (Ctrl+C to quit)...")
                    while client.is_connected:
                        await asyncio.sleep(1)
            except Exception as exc:
                print(f"Connection error: {exc}")

            print("Disconnected, rescanning...")
    finally:
        if csv_file:
            csv_file.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--csv", help="write every raw sample to this CSV file")
    args = parser.parse_args()

    try:
        asyncio.run(run(args.csv))
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
