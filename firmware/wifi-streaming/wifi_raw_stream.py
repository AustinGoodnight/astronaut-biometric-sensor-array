#!/usr/bin/env python3
"""High-rate raw sample receiver for the wifi-streaming board's binary TCP
server (port 3334).

Same wire format as the packet the old BLE raw stream used (2-byte sequence
number + RAW_BATCH_SIZE (ir, red) uint32 pairs, little-endian - must match
RawSample/RAW_BATCH_SIZE in src/main.cpp), but over a plain TCP socket, so
there's no BLE-connection-interval black box for the OS to throttle. Tracks
dropped batches via the sequence number and prints throughput once a second.
Pass --csv <path> to also write every sample to a CSV file.

Usage:
    python3 wifi_raw_stream.py <board-ip>
    python3 wifi_raw_stream.py <board-ip> --csv samples.csv
"""
import argparse
import csv
import socket
import struct
import sys
import time

RAW_PORT = 3334
RAW_BATCH_SIZE = 24  # must match RAW_BATCH_SIZE in src/main.cpp
PACKET_FORMAT = "<H" + "II" * RAW_BATCH_SIZE
PACKET_SIZE = struct.calcsize(PACKET_FORMAT)


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


def recv_exact(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed")
        buf += chunk
    return buf


def run(host: str, csv_path: str | None) -> None:
    stats = Stats()
    csv_writer = None
    csv_file = None
    if csv_path:
        csv_file = open(csv_path, "w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["seq", "sample_index", "ir", "red"])

    try:
        while True:
            print(f"Connecting to {host}:{RAW_PORT}...")
            try:
                with socket.create_connection((host, RAW_PORT), timeout=10) as sock:
                    print("Connected. Streaming raw samples (Ctrl+C to quit)...")
                    sock.settimeout(None)
                    while True:
                        packet = recv_exact(sock, PACKET_SIZE)
                        fields = struct.unpack(PACKET_FORMAT, packet)
                        seq = fields[0]
                        samples = fields[1:]
                        stats.record(seq)
                        if csv_writer:
                            for i in range(RAW_BATCH_SIZE):
                                ir, red = samples[2 * i], samples[2 * i + 1]
                                csv_writer.writerow([seq, i, ir, red])
                        stats.maybe_report()
            except (ConnectionError, OSError) as exc:
                print(f"Connection error: {exc}")

            print("Disconnected, retrying in 2s...")
            time.sleep(2)
    finally:
        if csv_file:
            csv_file.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host", help="the board's IP address (printed on serial at boot)")
    parser.add_argument("--csv", help="write every raw sample to this CSV file")
    args = parser.parse_args()

    try:
        run(args.host, args.csv)
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
