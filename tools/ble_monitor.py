#!/usr/bin/env python3
"""BLE/WiFi coexistence monitor for Wallbox ESP32 gateway.

Watches serial output and tracks:
- BLE scan results (device count, RSSI, found/not found)
- BLE connection attempts (success/fail, timing)
- BLE command flow (TX/RX, timeouts, response times)
- WiFi/MQTT events
- Keepalive pings
- Disconnects and reconnects

Usage:
    python ble_monitor.py [--port COM4] [--baud 115200]
"""

import argparse
import re
import serial
import sys
import time
from collections import defaultdict
from datetime import datetime


class WallboxMonitor:
    """Parses Wallbox gateway serial output line by line.

    Importable as a module:
        from ble_monitor import WallboxMonitor
        mon = WallboxMonitor()
        for line in serial_lines:
            mon.process_line(line)
        mon.print_summary()
    """
    def __init__(self):
        self.stats = {
            "scans": 0,
            "scan_found": 0,
            "scan_not_found": 0,
            "scan_devices_seen": [],
            "connects": 0,
            "connect_ok": 0,
            "connect_fail": 0,
            "commands_tx": 0,
            "commands_rx": 0,
            "timeouts": 0,
            "disconnects": 0,
            "pings": 0,
            "ping_fail": 0,
            "wifi_reconnects": 0,
            "mqtt_reconnects": 0,
            "last_rssi": None,
            "rssi_history": [],
            "uptime_start": time.time(),
        }
        self.last_tx_time = None
        self.last_tx_method = None
        self.response_times = []

    def process_line(self, line: str):
        ts = datetime.now().strftime("%H:%M:%S")
        line = line.strip()
        if not line:
            return

        # BLE Scan
        if "[BLE] Scanning for" in line:
            self.stats["scans"] += 1
            print(f"\033[36m{ts} SCAN\033[0m {line.split(']',1)[1].strip()}")

        elif "[BLE] Found!" in line:
            self.stats["scan_found"] += 1
            m = re.search(r"RSSI:\s*(-?\d+)", line)
            if m:
                rssi = int(m.group(1))
                self.stats["last_rssi"] = rssi
                self.stats["rssi_history"].append(rssi)
                if len(self.stats["rssi_history"]) > 100:
                    self.stats["rssi_history"] = self.stats["rssi_history"][-100:]
            print(f"\033[32m{ts} FOUND\033[0m {line.split(']',1)[1].strip()}")

        elif "[BLE] Not found" in line or "[BLE] Charger NOT" in line:
            self.stats["scan_not_found"] += 1
            m = re.search(r"\((\d+) devices", line)
            if m:
                self.stats["scan_devices_seen"].append(int(m.group(1)))
            print(f"\033[31m{ts} NOT FOUND\033[0m {line.split(']',1)[1].strip()}")

        # BLE Connect
        elif "[BLE] Connecting" in line:
            self.stats["connects"] += 1
            print(f"\033[33m{ts} CONNECT\033[0m {line.split(']',1)[1].strip()}")

        elif "[BLE] Connected" in line or "[BLE] Ready" in line:
            self.stats["connect_ok"] += 1
            print(f"\033[32m{ts} OK\033[0m {line.split(']',1)[1].strip()}")

        elif "[BLE] Connection failed" in line:
            self.stats["connect_fail"] += 1
            print(f"\033[31m{ts} FAIL\033[0m {line.split(']',1)[1].strip()}")

        elif "[BLE] Connection lost" in line:
            self.stats["disconnects"] += 1
            print(f"\033[31m{ts} DISCONNECT\033[0m {line.split(']',1)[1].strip()}")

        # BLE Commands
        elif "[BLE] TX " in line:
            self.stats["commands_tx"] += 1
            self.last_tx_time = time.time()
            m = re.search(r"TX (\S+)", line)
            self.last_tx_method = m.group(1) if m else "?"
            if self.last_tx_method == "ping":
                self.stats["pings"] += 1

        elif "[BLE] RX notify" in line:
            self.stats["commands_rx"] += 1
            if self.last_tx_time:
                rt = (time.time() - self.last_tx_time) * 1000
                self.response_times.append(rt)
                if len(self.response_times) > 100:
                    self.response_times = self.response_times[-100:]

        elif "[BLE] Timeout" in line:
            self.stats["timeouts"] += 1
            m = re.search(r"for (\S+)", line)
            method = m.group(1) if m else "?"
            if method == "ping":
                self.stats["ping_fail"] += 1
            print(f"\033[31m{ts} TIMEOUT\033[0m {method}")

        elif "[BLE] Ping timeout" in line:
            self.stats["ping_fail"] += 1
            print(f"\033[31m{ts} PING DEAD\033[0m {line.split(']',1)[1].strip()}")

        # WiFi / MQTT
        elif "[WiFi] Reconnecting" in line:
            self.stats["wifi_reconnects"] += 1
            print(f"\033[33m{ts} WIFI\033[0m Reconnecting")

        elif "[MQTT] Connecting" in line:
            self.stats["mqtt_reconnects"] += 1

        elif "[MQTT] Connected" in line:
            print(f"\033[32m{ts} MQTT\033[0m Connected")

        # Radio coexistence
        elif "[Radio]" in line or "Coexistence" in line:
            print(f"\033[35m{ts} RADIO\033[0m {line.split(']',1)[1].strip()}")

        # Connection params
        elif "Connection params" in line or "updateConnParams" in line:
            print(f"\033[35m{ts} PARAMS\033[0m {line.split(']',1)[1].strip()}")

        # Errors
        elif "error" in line.lower() and "[BLE]" in line:
            print(f"\033[31m{ts} ERROR\033[0m {line.split(']',1)[1].strip()}")

    def print_summary(self):
        s = self.stats
        uptime = time.time() - s["uptime_start"]
        mins = int(uptime / 60)

        avg_rssi = sum(s["rssi_history"]) / len(s["rssi_history"]) if s["rssi_history"] else 0
        avg_rt = sum(self.response_times) / len(self.response_times) if self.response_times else 0

        scan_rate = s["scan_found"] / s["scans"] * 100 if s["scans"] > 0 else 0
        conn_rate = s["connect_ok"] / s["connects"] * 100 if s["connects"] > 0 else 0
        timeout_rate = s["timeouts"] / s["commands_tx"] * 100 if s["commands_tx"] > 0 else 0

        print("\n" + "=" * 60)
        print(f"  BLE Monitor Summary ({mins}m)")
        print("=" * 60)
        print(f"  Scans:      {s['scans']} total, {s['scan_found']} found ({scan_rate:.0f}%), {s['scan_not_found']} missed")
        print(f"  Connects:   {s['connects']} attempts, {s['connect_ok']} ok ({conn_rate:.0f}%), {s['connect_fail']} fail")
        print(f"  Disconnects:{s['disconnects']}")
        print(f"  Commands:   {s['commands_tx']} TX, {s['commands_rx']} RX, {s['timeouts']} timeouts ({timeout_rate:.0f}%)")
        print(f"  Pings:      {s['pings']} sent, {s['ping_fail']} failed")
        print(f"  RSSI:       last={s['last_rssi']} avg={avg_rssi:.0f} (n={len(s['rssi_history'])})")
        print(f"  Response:   avg={avg_rt:.0f}ms (n={len(self.response_times)})")
        print(f"  WiFi drops: {s['wifi_reconnects']}")
        print(f"  MQTT drops: {s['mqtt_reconnects']}")

        if s["scan_devices_seen"]:
            avg_devs = sum(s["scan_devices_seen"]) / len(s["scan_devices_seen"])
            print(f"  BLE devices: avg {avg_devs:.0f} per scan")

        # Diagnosis
        print("\n  Diagnosis:")
        if s["scan_not_found"] > 0 and s["scan_found"] == 0:
            print("  ⚠ Charger never found — check range/antenna/charger power")
        elif scan_rate < 50:
            print("  ⚠ Low scan hit rate — charger may be sleeping or at edge of range")
        if s["connect_fail"] > s["connect_ok"]:
            print("  ⚠ More failures than successes — weak signal or interference")
        if timeout_rate > 20:
            print("  ⚠ High timeout rate — BLE/WiFi coexistence issue likely")
        if s["wifi_reconnects"] > 2:
            print("  ⚠ WiFi unstable — may be fighting BLE for radio")
        if s["ping_fail"] > 0:
            print("  ⚠ Ping failures — connection dying between polls")
        if avg_rssi < -85 and s["rssi_history"]:
            print(f"  ⚠ Weak signal (avg {avg_rssi:.0f} dBm) — move ESP32 closer")
        if s["disconnects"] == 0 and s["connect_ok"] > 0 and timeout_rate < 5:
            print("  ✓ BLE looks stable!")

        print("=" * 60)


def main():
    parser = argparse.ArgumentParser(description="Wallbox BLE/WiFi monitor")
    parser.add_argument("--port", required=True,
        help="Serial port (e.g. COM4 on Windows, /dev/ttyUSB0 on Linux)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate")
    args = parser.parse_args()

    monitor = WallboxMonitor()

    print(f"Monitoring {args.port} @ {args.baud}...")
    print("Press Ctrl+C for summary\n")

    try:
        ser = serial.Serial(args.port, args.baud, timeout=1)
        while True:
            line = ser.readline()
            if line:
                text = line.decode("utf-8", errors="replace").strip()
                if text:
                    monitor.process_line(text)
    except KeyboardInterrupt:
        monitor.print_summary()
    except serial.SerialException as e:
        print(f"Serial error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
