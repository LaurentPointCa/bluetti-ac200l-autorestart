# AC200L BLE PoC — read-only telemetry test

This is the gating test for the auto-restart project. If this connects to the AC200L
over Bluetooth and reads telemetry cleanly, the Bluetooth control path is viable and we
proceed to (1) testing the AC-output toggle, then (2) a Raspberry Pi that re-arms AC
output when grid power returns.

`ac200l_read.py` is **strictly read-only** — it only issues Modbus read commands and
never writes, so it cannot change a setting or toggle an output.

## Before you run

- AC200L **powered on** and within Bluetooth range of the Mac.
- The **BLUETTI phone app fully closed / disconnected**. The unit allows only ONE
  Bluetooth connection at a time, so the app and this script can't both be connected.

## Setup (one time)

```sh
cd ~/claude/bluetti/poc
python3 -m venv .venv
source .venv/bin/activate
pip install bleak
```

## Run

```sh
source .venv/bin/activate          # if not already active
python ac200l_read.py              # scan, connect, poll every 5s (Ctrl+C to stop)
python ac200l_read.py --once       # single read then exit
python ac200l_read.py --scan-only  # just list nearby BLE devices (no connect)
```

macOS will pop a **Bluetooth permission** prompt for the terminal the first time — allow it.

## What good output looks like

```
Found: AC200L2403xxxxxxxx  [E3A1...-UUID]
Connecting...
Connected. Subscribing to notifications.

14:02:11  SoC  87%  |  AC-in  142W (GRID PRESENT)  |  AC-out  118W (AC OUTPUT ON)  |  DC off
14:02:16  SoC  87%  |  AC-in  141W (GRID PRESENT)  |  AC-out  120W (AC OUTPUT ON)  |  DC off
```

## What to report back to me

Copy the output, especially:

1. Did it **find and connect**? (the `Found:` / `Connected.` lines)
2. Do the **SoC / AC-in / AC-out** numbers look correct vs the unit's screen?
3. Any **errors** — `device not found`, timeouts, `CRC mismatch`, `unexpected length`,
   or a Modbus error. Those tell me whether to adjust the read range or framing.

If it can't find the device, run `--scan-only` and send me the list — I need the exact
advertised name to match on (we may need to change the `--name` prefix).

## Notes / known quirks

- This passes the discovered BLE device object straight to `bleak`, which avoids the
  macOS "address is a CoreBluetooth UUID, not a MAC" gotcha.
- If reads time out but the device is found, the most likely cause is the read register
  range (`READ_START` / `READ_COUNT` near the top of the script). Some firmware only
  answers specific register pages — send me the error and I'll adjust the range.
- The AC200L is **not** using Bluetti's newer encrypted BLE protocol on current firmware,
  so no key/handshake is needed. If a future firmware enables it, telemetry would stop
  and we'd see disconnects — tell me if that ever happens.

## Next steps after this works

1. Add a **write** test: toggle AC output via control register 3007 (function 0x06),
   confirmed working on the AC200L by the ftrueck fork. (Separate, clearly-guarded script.)
2. Port to a **Raspberry Pi** (built-in BLE), powered from a **grid wall outlet** so it
   boots when grid returns, sitting in BLE range of the unit. It detects grid-return
   (AC-in > 0) + SoC recovered + AC-output still off, then re-arms AC output. Retries
   until the flaky BLE cooperates — no deadline, because the unit auto-wakes and charges
   on its own after a full drain; it just won't re-enable output without this nudge.
