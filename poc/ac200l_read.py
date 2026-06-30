#!/usr/bin/env python3
"""
AC200L read-only Bluetooth LE proof-of-concept.

Goal: confirm we can connect to the AC200L over BLE from this Mac and read its
live telemetry (state of charge, grid-input power, AC-output status). This is the
gating test for the auto-restart project: if this connects and reads cleanly, the
Bluetooth control path is viable.

STRICTLY READ-ONLY. It only issues Modbus "read holding registers" (function 0x03)
commands. It never writes, so it cannot change any setting or toggle any output.

Requires:
  - The AC200L powered on and within Bluetooth range of this Mac.
  - The BLUETTI phone app fully closed / disconnected. The unit accepts only ONE
    Bluetooth connection at a time, so the app and this script are mutually exclusive.

Setup (one time):
  python3 -m venv .venv
  source .venv/bin/activate
  pip install bleak

Run:
  python ac200l_read.py                 # scan, connect, poll every 5s
  python ac200l_read.py --name AC200L   # override device-name prefix to match on
  python ac200l_read.py --once          # single read then exit
  python ac200l_read.py --scan-only     # just list nearby BLE devices and exit

Protocol facts (from warhammerkid/bluetti_mqtt + ftrueck AC200L fork):
  Write char : 0000ff02-0000-1000-8000-00805f9b34fb  (commands go here)
  Notify char: 0000ff01-0000-1000-8000-00805f9b34fb  (responses arrive here)
  Frame      : [0x01 addr][0x03 func][start:2 BE][qty:2 BE][CRC16 modbus, LE]
  Response   : [addr][func][bytecount] + data(2*qty) + CRC16(LE)
  Registers  : 37 ac_input_power(W)  38 ac_output_power(W)  43 total_battery_percent(%)
               48 ac_output_on(bool)  49 dc_output_on(bool)
"""

import argparse
import asyncio
import struct
import sys
from datetime import datetime

try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    sys.exit("Missing dependency. Run:  pip install bleak")

WRITE_UUID = "0000ff02-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"

# Single contiguous read covering every field we care about: registers 36..49.
READ_START = 36
READ_COUNT = 14  # 36,37,...,49 inclusive
RESPONSE_TIMEOUT = 5.0


def modbus_crc(data: bytes) -> int:
    """Standard Modbus CRC-16 (poly 0xA001, init 0xFFFF)."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def build_read_frame(start: int, count: int) -> bytes:
    body = struct.pack(">BBHH", 0x01, 0x03, start, count)
    return body + struct.pack("<H", modbus_crc(body))


def parse_response(resp: bytes, count: int) -> bytes:
    """Validate a function-0x03 response and return the raw register data bytes."""
    if len(resp) < 5:
        raise ValueError(f"response too short: {resp.hex()}")
    if resp[1] & 0x80:
        raise ValueError(f"device returned Modbus error (func 0x{resp[1]:02x}, "
                         f"exception 0x{resp[2]:02x})")
    expected = 2 * count + 5
    if len(resp) != expected:
        raise ValueError(f"unexpected length {len(resp)} (wanted {expected}): {resp.hex()}")
    got_crc = int.from_bytes(resp[-2:], "little")
    if got_crc != modbus_crc(resp[:-2]):
        raise ValueError(f"CRC mismatch: {resp.hex()}")
    return bytes(resp[3:-2])


def reg(data: bytes, addr: int, start: int) -> int:
    off = (addr - start) * 2
    return int.from_bytes(data[off:off + 2], "big")


async def find_device(name_prefix: str, timeout: float = 12.0):
    print(f"Scanning for a BLE device whose name starts with '{name_prefix}' "
          f"({timeout:.0f}s)...")
    devices = await BleakScanner.discover(timeout=timeout)
    matches = [d for d in devices if (d.name or "").upper().startswith(name_prefix.upper())]
    if not matches:
        print("No match. Nearby named devices were:")
        for d in sorted(devices, key=lambda x: x.name or "~"):
            if d.name:
                print(f"  {d.name}  [{d.address}]")
        return None
    dev = matches[0]
    print(f"Found: {dev.name}  [{dev.address}]")
    return dev


class Reader:
    def __init__(self):
        self.buf = bytearray()
        self.done = asyncio.Event()

    def _on_notify(self, _char, data: bytearray):
        self.buf.extend(data)
        if len(self.buf) >= 2 * READ_COUNT + 5:
            self.done.set()

    async def read_once(self, client: BleakClient) -> dict:
        self.buf.clear()
        self.done.clear()
        await client.write_gatt_char(WRITE_UUID, build_read_frame(READ_START, READ_COUNT),
                                     response=False)
        await asyncio.wait_for(self.done.wait(), timeout=RESPONSE_TIMEOUT)
        data = parse_response(bytes(self.buf), READ_COUNT)
        return {
            "soc_pct": reg(data, 43, READ_START),
            "ac_input_w": reg(data, 37, READ_START),
            "ac_output_w": reg(data, 38, READ_START),
            "ac_output_on": bool(reg(data, 48, READ_START) & 1),
            "dc_output_on": bool(reg(data, 49, READ_START) & 1),
        }


def render(s: dict) -> str:
    grid = "GRID PRESENT" if s["ac_input_w"] > 0 else "OUTAGE (no AC input)"
    out = "AC OUTPUT ON" if s["ac_output_on"] else "AC output OFF"
    return (f"SoC {s['soc_pct']:3d}%  |  AC-in {s['ac_input_w']:4d}W ({grid})  |  "
            f"AC-out {s['ac_output_w']:4d}W ({out})  |  DC {'on' if s['dc_output_on'] else 'off'}")


async def main():
    ap = argparse.ArgumentParser(description="AC200L read-only BLE telemetry PoC")
    ap.add_argument("--name", default="AC200L", help="device-name prefix to match (default AC200L)")
    ap.add_argument("--interval", type=float, default=5.0, help="seconds between reads")
    ap.add_argument("--once", action="store_true", help="read once and exit")
    ap.add_argument("--scan-only", action="store_true", help="list nearby BLE devices and exit")
    args = ap.parse_args()

    if args.scan_only:
        devices = await BleakScanner.discover(timeout=12.0)
        for d in sorted(devices, key=lambda x: x.name or "~"):
            print(f"  {d.name or '(unnamed)'}  [{d.address}]")
        return

    dev = await find_device(args.name)
    if dev is None:
        sys.exit("Device not found. Is the AC200L on, in range, and the phone app closed?")

    print("Connecting...")
    async with BleakClient(dev) as client:
        print("Connected. Subscribing to notifications.\n")
        reader = Reader()
        await client.start_notify(NOTIFY_UUID, reader._on_notify)
        try:
            while True:
                try:
                    s = await reader.read_once(client)
                    print(f"{datetime.now():%H:%M:%S}  {render(s)}")
                except asyncio.TimeoutError:
                    print(f"{datetime.now():%H:%M:%S}  (no response within "
                          f"{RESPONSE_TIMEOUT:.0f}s — retrying)")
                except ValueError as e:
                    print(f"{datetime.now():%H:%M:%S}  parse error: {e}")
                if args.once:
                    break
                await asyncio.sleep(args.interval)
        finally:
            await client.stop_notify(NOTIFY_UUID)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
