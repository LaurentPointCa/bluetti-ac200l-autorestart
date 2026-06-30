# Bluetti AC200L auto-restart

Make a Bluetti **AC200L** used as a UPS recover on its own after a power outage drains it.

## The problem

In UPS mode, if grid power fails and the outage lasts long enough to drain the AC200L to 0%,
the unit shuts down. When grid power returns the unit **auto-wakes and recharges**, but it does
**not** re-enable its AC output — so everything plugged in (e.g. your home internet gear) stays
dead until someone manually turns AC output back on. Bluetti declined to add an auto-restart
setting (citing safety), and the native UPS "SoC low" floor does **not** help: it's ignored
during a real outage, so the unit still drains to 0.

## The fix

A tiny always-on controller watches the unit over Bluetooth LE and re-enables AC output
whenever **grid power is present but AC output is off** — exactly the post-outage dead-latch
state. It's the one nudge the unit won't do itself.

Why this works and stays robust:
- The AC200L auto-wakes and recharges when grid returns, and is controllable again over BLE,
  so the controller can re-arm at its leisure — no racing the clock.
- When grid is present, AC output runs from grid pass-through, so re-arming is safe at any SoC.
- The controller is **fully self-contained over direct Bluetooth** — no cloud, no Home
  Assistant, no LAN dependency. That matters because the AC200L powers the internet gear, so
  there is no network until the unit is re-armed (rules out cloud and ESP32-BLE-proxy designs).
- Power the controller from a **grid wall outlet** (not the AC200L) so it's alive exactly when
  grid power is available.

## What's here

| Path | What it is |
|------|------------|
| `esp32/` | **Recommended deployable controller.** ESP32 firmware (NimBLE-Arduino, PlatformIO). Standalone; optional WiFi optimization (stays off Bluetooth while the network is up, so the phone app keeps working). |
| `controller/` | Alternative: Python daemon + systemd unit for a Raspberry Pi. Same logic. |
| `poc/` | Mac proof-of-concept scripts (`bleak`) that reverse-validated the protocol on real hardware: read telemetry and the AC-output re-arm write. |

Each folder has its own README with build/flash/run/deploy steps.

## Protocol (validated on real AC200L hardware)

Unencrypted Modbus over BLE GATT. Write char `0000ff02-...`, notify char `0000ff01-...`.
- Read holding registers (func `0x03`), range 36..49: `37` ac_input_power (W, grid-present
  proxy), `38` ac_output_power (W), `43` SoC (%), `48` ac_output_on (bool).
- Re-arm: write single register (func `0x06`) `3007` = `1`.

## Status

Read + write both proven on a real AC200L. ESP32 firmware compiles clean. Remaining step is to
flash a board and deploy it. See `esp32/README.md`.

## Residual risks

- The rare AC200L firmware hang where buttons **and** Bluetooth freeze until a manual power
  cycle — no controller can fix that.
- A future firmware that switches the AC200L to Bluetti's encrypted BLE protocol would require
  the encrypted handshake (current firmware is unencrypted).
