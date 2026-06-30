#!/usr/bin/env python3
"""
AC200L AC-output write test (the re-arm command).

This proves the single write our auto-restart controller depends on: toggling AC
output via Modbus "write single register" (function 0x06) to control register 3007.
Status is read back from register 48 to confirm the change took effect.

  *** THIS CHANGES THE AC OUTPUT. ***
  Setting it OFF cuts power to everything plugged into the AC outlets. Setting it ON
  restores it. Test with nothing critical connected, or when a brief interruption to
  your connected gear is acceptable.

Safety design:
  - With no --yes flag it does a DRY RUN: it reads current state and prints the exact
    bytes it WOULD send, but writes nothing.
  - --set on|off and --yes are both required to actually write.

Setup: same venv as the reader (pip install bleak).

Examples:
  python ac200l_write.py --set off            # dry run: show what turning OFF would send
  python ac200l_write.py --set off --yes      # actually turn AC output OFF
  python ac200l_write.py --set on  --yes      # actually turn AC output ON (the re-arm)
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

AC_OUTPUT_CTRL_REG = 3007   # writable control register for AC output
AC_OUTPUT_STATUS_REG = 48   # read-only status register for AC output
RESPONSE_TIMEOUT = 5.0


def modbus_crc(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def build_read_frame(start: int, count: int) -> bytes:
    body = struct.pack(">BBHH", 0x01, 0x03, start, count)
    return body + struct.pack("<H", modbus_crc(body))


def build_write_frame(reg: int, value: int) -> bytes:
    """Modbus write-single-register (func 0x06)."""
    body = struct.pack(">BBHH", 0x01, 0x06, reg, value)
    return body + struct.pack("<H", modbus_crc(body))


async def find_device(name_prefix: str, timeout: float = 12.0):
    print(f"Scanning for '{name_prefix}*' ({timeout:.0f}s)...")
    devices = await BleakScanner.discover(timeout=timeout)
    matches = [d for d in devices if (d.name or "").upper().startswith(name_prefix.upper())]
    if not matches:
        print("No match. Nearby named devices:")
        for d in sorted(devices, key=lambda x: x.name or "~"):
            if d.name:
                print(f"  {d.name}  [{d.address}]")
        return None
    print(f"Found: {matches[0].name}  [{matches[0].address}]")
    return matches[0]


class Conn:
    def __init__(self):
        self.buf = bytearray()
        self.want = 0
        self.done = asyncio.Event()

    def _on_notify(self, _char, data: bytearray):
        self.buf.extend(data)
        if self.want and len(self.buf) >= self.want:
            self.done.set()

    async def _txn(self, frame: bytes, want: int) -> bytes:
        self.buf.clear()
        self.want = want
        self.done.clear()
        await self.client.write_gatt_char(WRITE_UUID, frame, response=False)
        await asyncio.wait_for(self.done.wait(), timeout=RESPONSE_TIMEOUT)
        return bytes(self.buf)

    async def read_ac_output(self) -> bool:
        # read single status register 48; response = addr,func,bytecount(1),data(2),crc(2) = 7
        resp = await self._txn(build_read_frame(AC_OUTPUT_STATUS_REG, 1), 7)
        if len(resp) < 7 or resp[1] & 0x80:
            raise ValueError(f"bad read response: {resp.hex()}")
        return bool(int.from_bytes(resp[3:5], "big") & 1)

    async def write_ac_output(self, on: bool) -> bytes:
        # func 0x06 response echoes the 8-byte request
        return await self._txn(build_write_frame(AC_OUTPUT_CTRL_REG, 1 if on else 0), 8)


async def main():
    ap = argparse.ArgumentParser(description="AC200L AC-output write test")
    ap.add_argument("--name", default="AC200L")
    ap.add_argument("--set", choices=["on", "off"], required=True, help="target AC output state")
    ap.add_argument("--yes", action="store_true",
                    help="actually write (without this it is a dry run)")
    args = ap.parse_args()
    want_on = args.set == "on"

    dev = await find_device(args.name)
    if dev is None:
        sys.exit("Device not found. Is the AC200L on, in range, and the phone app closed?")

    async with BleakClient(dev) as client:
        c = Conn()
        c.client = client
        await client.start_notify(NOTIFY_UUID, c._on_notify)
        try:
            before = await c.read_ac_output()
            print(f"{datetime.now():%H:%M:%S}  AC output is currently: "
                  f"{'ON' if before else 'OFF'}")

            frame = build_write_frame(AC_OUTPUT_CTRL_REG, 1 if want_on else 0)
            if not args.yes:
                print(f"\nDRY RUN — would write to set AC output {args.set.upper()}:")
                print(f"  reg {AC_OUTPUT_CTRL_REG} = {1 if want_on else 0}")
                print(f"  bytes: {frame.hex()}")
                print("\nNo write performed. Re-run with --yes to actually do it.")
                return

            if before == want_on:
                print(f"Already {args.set.upper()}; nothing to do.")
                return

            print(f"*** Writing: set AC output {args.set.upper()} "
                  f"(this interrupts/restores AC power now) ***")
            echo = await c.write_ac_output(want_on)
            print(f"  write echo: {echo.hex()}")
            await asyncio.sleep(1.5)
            after = await c.read_ac_output()
            print(f"{datetime.now():%H:%M:%S}  AC output is now: {'ON' if after else 'OFF'}")
            print("SUCCESS" if after == want_on else "WARNING: state did not change as expected")
        finally:
            await client.stop_notify(NOTIFY_UUID)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopped.")
