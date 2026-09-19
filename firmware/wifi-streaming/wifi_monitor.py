#!/usr/bin/env python3
"""Text log monitor for the wifi-streaming board's log TCP server (port 3333).

Wireless equivalent of `pio device monitor`, but over WiFi instead of BLE -
plain TCP, no per-connection throttling to fight (see CLAUDE.md for why BLE
on macOS made this unreliable). Reconnects automatically if the board resets
or the connection drops.

Usage:
    python3 wifi_monitor.py <board-ip>
"""
import socket
import sys
import time

LOG_PORT = 3333


def run(host: str) -> None:
    while True:
        print(f"Connecting to {host}:{LOG_PORT}...")
        try:
            with socket.create_connection((host, LOG_PORT), timeout=10) as sock:
                print("Connected. Streaming log lines (Ctrl+C to quit)...")
                sock.settimeout(None)
                buf = b""
                while True:
                    chunk = sock.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        print(line.decode(errors="replace").rstrip("\r"))
        except (ConnectionError, OSError) as exc:
            print(f"Connection error: {exc}")

        print("Disconnected, retrying in 2s...")
        time.sleep(2)


def main() -> None:
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <board-ip>", file=sys.stderr)
        sys.exit(1)

    try:
        run(sys.argv[1])
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
