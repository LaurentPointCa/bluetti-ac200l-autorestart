#!/usr/bin/env python3
"""
AC200L auto-restart controller.

Solves: when a power outage drains the AC200L to 0%, the unit shuts down; when grid
returns it auto-wakes and recharges but does NOT re-enable AC output on its own, so
everything plugged in (the home internet gear) stays dead until someone intervenes.

This daemon watches the unit over Bluetooth LE and re-enables AC output automatically
whenever grid power is present but AC output is off. It is meant to run on a small
always-on box (Raspberry Pi) powered from a GRID wall outlet (NOT from the AC200L), so
it is alive exactly when grid power is available — which is the only time it can or
should act.

Why "grid present + output off -> turn output on" is the right rule:
  - When grid is present, AC output runs from grid pass-through, so enabling it is safe
    regardless of battery SoC.
  - After a full-drain outage the unit comes back charging (grid present) with output
    off -> this rule re-arms it.
  - During an outage (grid absent) the rule does nothing, so it never fights the unit's
    own battery-protection shutdown.
  - Caveat: if you deliberately switch AC output off while grid is present, this will
    turn it back on within one poll. For a dedicated internet-UPS that is the desired
    behavior. Set --require-recovery to instead only act after observing an outage.

Protocol (proven on real AC200L hardware):
  Write char 0000ff02-..., Notify char 0000ff01-...; Modbus over GATT.
  Read range 36..49 (func 0x03): 37 ac_input_power(W), 43 SoC(%), 48 ac_output_on(bool).
  Re-arm: func 0x06 write reg 3007 = 1.

Run (foreground, for testing on Mac or Pi):
  python ac200l_autorestart.py
  python ac200l_autorestart.py --dry-run        # never writes; logs what it would do
  python ac200l_autorestart.py --interval 30
Deploy: see controller/DEPLOY.md (systemd unit included).
"""

from __future__ import annotations  # allow "str | None" hints on Python 3.9

import argparse
import asyncio
import logging
import struct
import sys
import time

try:
    from bleak import BleakClient, BleakScanner
    from bleak.exc import BleakError
except ImportError:
    sys.exit("Missing dependency. Run:  pip install bleak")

WRITE_UUID = "0000ff02-0000-1000-8000-00805f9b34fb"
NOTIFY_UUID = "0000ff01-0000-1000-8000-00805f9b34fb"

READ_START, READ_COUNT = 36, 14          # covers regs 36..49
AC_OUTPUT_CTRL_REG = 3007
RESPONSE_TIMEOUT = 5.0
SCAN_TIMEOUT = 12.0

# Back-off / safety: if a re-arm does not "stick" repeatedly, stop hammering and warn.
MAX_FAILED_REARMS = 5            # consecutive failed re-arms before long back-off
BACKOFF_AFTER_FAIL = 1800.0     # seconds to wait after hitting the failure ceiling
REARM_GRACE = 90.0              # seconds to leave it alone after a successful re-arm

log = logging.getLogger("ac200l")


def modbus_crc(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def read_frame(start: int, count: int) -> bytes:
    body = struct.pack(">BBHH", 0x01, 0x03, start, count)
    return body + struct.pack("<H", modbus_crc(body))


def write_frame(reg: int, value: int) -> bytes:
    body = struct.pack(">BBHH", 0x01, 0x06, reg, value)
    return body + struct.pack("<H", modbus_crc(body))


def reg(data: bytes, addr: int) -> int:
    off = (addr - READ_START) * 2
    return int.from_bytes(data[off:off + 2], "big")


class Unit:
    """A single connect/transact/disconnect session to the AC200L."""

    def __init__(self, client: BleakClient):
        self.client = client
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

    async def read_state(self) -> dict:
        resp = await self._txn(read_frame(READ_START, READ_COUNT), 2 * READ_COUNT + 5)
        if len(resp) < 5 or resp[1] & 0x80:
            raise ValueError(f"bad read response: {resp.hex()}")
        if int.from_bytes(resp[-2:], "little") != modbus_crc(resp[:-2]):
            raise ValueError(f"CRC mismatch: {resp.hex()}")
        data = resp[3:-2]
        return {
            "soc": reg(data, 43),
            "ac_in": reg(data, 37),
            "ac_out_w": reg(data, 38),
            "ac_output_on": bool(reg(data, 48) & 1),
        }

    async def set_ac_output(self, on: bool) -> None:
        await self._txn(write_frame(AC_OUTPUT_CTRL_REG, 1 if on else 0), 8)


async def resolve_address(name_prefix: str, cached: str | None):
    """Return a connect target: reuse the cached address, else scan by name."""
    if cached:
        return cached
    devices = await BleakScanner.discover(timeout=SCAN_TIMEOUT)
    for d in devices:
        if (d.name or "").upper().startswith(name_prefix.upper()):
            log.info("found %s [%s]", d.name, d.address)
            return d.address
    return None


async def run(args):
    cached_addr: str | None = None
    failed_rearms = 0
    grace_until = 0.0
    saw_outage = False  # only used when --require-recovery

    while True:
        try:
            addr = await resolve_address(args.name, cached_addr)
            if addr is None:
                log.warning("AC200L not found (is it on and in range?); retrying")
                cached_addr = None
                await asyncio.sleep(args.interval)
                continue

            async with BleakClient(addr) as client:
                cached_addr = addr
                unit = Unit(client)
                await client.start_notify(NOTIFY_UUID, unit._on_notify)
                try:
                    s = await unit.read_state()
                    grid = s["ac_in"] > 0
                    log.info("SoC %d%% | AC-in %dW (%s) | AC-out %dW (%s)",
                             s["soc"], s["ac_in"], "grid" if grid else "OUTAGE",
                             s["ac_out_w"], "on" if s["ac_output_on"] else "OFF")

                    if not grid:
                        saw_outage = True

                    now = time.monotonic()
                    need = grid and not s["ac_output_on"]
                    if args.require_recovery:
                        need = need and saw_outage

                    if need and now < grace_until:
                        log.info("re-arm condition met but within grace period; waiting")
                    elif need and failed_rearms >= MAX_FAILED_REARMS:
                        if now < grace_until:
                            pass
                        else:
                            log.error("re-arm failed %d times; backing off %.0fs",
                                      failed_rearms, BACKOFF_AFTER_FAIL)
                            grace_until = now + BACKOFF_AFTER_FAIL
                            failed_rearms = 0
                    elif need:
                        if args.dry_run:
                            log.warning("[dry-run] would re-arm AC output now")
                        else:
                            log.warning("grid present + AC output OFF -> re-arming AC output")
                            await unit.set_ac_output(True)
                            await asyncio.sleep(2.0)
                            after = await unit.read_state()
                            if after["ac_output_on"]:
                                log.warning("re-arm SUCCESS (AC output now ON)")
                                failed_rearms = 0
                                grace_until = now + REARM_GRACE
                                saw_outage = False
                            else:
                                failed_rearms += 1
                                log.error("re-arm did not stick (attempt %d)", failed_rearms)
                    else:
                        # healthy: grid present + output on, or genuine outage. Reset counters.
                        if grid and s["ac_output_on"]:
                            failed_rearms = 0
                finally:
                    await client.stop_notify(NOTIFY_UUID)

        except (asyncio.TimeoutError, BleakError, ValueError, OSError) as e:
            log.warning("poll failed (%s: %s); will retry", type(e).__name__, e)
            cached_addr = None  # force a fresh scan next time
        except Exception:  # noqa: BLE stacks raise odd things; never let the daemon die
            log.exception("unexpected error; continuing")
            cached_addr = None

        await asyncio.sleep(args.interval)


def main():
    ap = argparse.ArgumentParser(description="AC200L auto-restart controller")
    ap.add_argument("--name", default="AC200L", help="BLE device-name prefix (default AC200L)")
    ap.add_argument("--interval", type=float, default=60.0, help="seconds between polls")
    ap.add_argument("--dry-run", action="store_true", help="never write; log intended actions")
    ap.add_argument("--require-recovery", action="store_true",
                    help="only re-arm after observing an outage (avoids overriding a "
                         "deliberate manual off); note: a wall-powered Pi reboots on grid "
                         "return and loses this memory, so the default rule is usually better")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )
    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        log.info("stopped")


if __name__ == "__main__":
    main()
