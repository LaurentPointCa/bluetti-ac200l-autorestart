# AC200L auto-restart controller — deployment

The controller watches the AC200L over Bluetooth and re-enables AC output whenever
**grid power is present but AC output is off** — which is exactly the state the unit
gets stuck in after a full-drain outage. The unit revives and recharges itself when grid
returns; this just provides the one nudge it won't do on its own: turning AC output back on.

## The governing constraint (why it's built this way)

The AC200L powers the home internet gear, so during/after an outage there is **no LAN and
no cloud** until the unit re-arms. Therefore the controller must be **fully self-contained
and talk Bluetooth directly to the unit** — no Home Assistant + ESP32 proxy (needs the LAN),
no cloud. And it should be **powered from a grid wall outlet, NOT from the AC200L**, so it
boots exactly when grid returns (the boot itself signals "grid is back") and isn't drained
along with the unit.

## Hardware

- **Raspberry Pi Zero 2 W** — built-in Bluetooth LE, cheap, low power. (Any Pi 3/4/5 works
  too; all have BLE. An old always-on Linux box with BLE is fine as well.)
- microSD card (8 GB+), and a USB power supply plugged into a **wall outlet on grid power**.
- Place it within Bluetooth range of the AC200L (same room / a few meters).

No BLE dongle needed — the Pi's built-in radio is enough.

## Test on the Mac first (optional but recommended)

The daemon is portable (uses `bleak`). Before deploying, you can run it on the Mac:

```sh
cd ~/claude/bluetti/controller
source ../poc/.venv/bin/activate     # reuse the PoC venv (has bleak)
python ac200l_autorestart.py --dry-run            # logs decisions, never writes
# then, to see a real re-arm: turn AC output off via the app or the write PoC, and run:
python ac200l_autorestart.py --interval 15        # it should detect + re-arm within a poll
```

`--dry-run` is safe: it logs "would re-arm" but never writes.

## Pi setup

1. Flash **Raspberry Pi OS Lite (64-bit)** with Raspberry Pi Imager. In the imager's
   settings, enable SSH and set Wi-Fi + hostname so it's headless. (Wi-Fi is only used for
   you to SSH in and for optional alerts — the re-arm itself does not need the network.)

2. SSH in and install Python + Bluetooth bits (Raspberry Pi OS already has BlueZ):

   ```sh
   sudo apt update && sudo apt install -y python3-venv python3-pip
   mkdir -p ~/ac200l && cd ~/ac200l
   ```

3. Copy the controller onto the Pi (from your Mac):

   ```sh
   scp ~/claude/bluetti/controller/ac200l_autorestart.py pi@<pi-host>:~/ac200l/
   scp ~/claude/bluetti/controller/ac200l-autorestart.service pi@<pi-host>:~/ac200l/
   ```

4. Create the venv and install bleak on the Pi:

   ```sh
   cd ~/ac200l
   python3 -m venv .venv
   .venv/bin/pip install bleak
   ```

5. Quick manual test (with the phone app closed):

   ```sh
   .venv/bin/python ac200l_autorestart.py --interval 15 --verbose
   ```

   You should see it find the unit and log SoC / AC-in / AC-out each poll. Turn AC output
   off (app or write PoC) and confirm it re-arms within a poll. Ctrl+C to stop.

6. Install as a service so it runs on boot and restarts on failure:

   ```sh
   sudo cp ac200l-autorestart.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now ac200l-autorestart
   journalctl -u ac200l-autorestart -f      # watch the logs
   ```

   If your Pi username isn't `pi`, edit `User=` and the paths in the `.service` file first.

## Behavior notes

- **Single Bluetooth connection.** While the daemon is running it holds the unit's only BLE
  slot, so the **phone Bluetti app can't connect** at the same time. Stop the service
  (`sudo systemctl stop ac200l-autorestart`) when you want to use the app, then start it again.
- **Deliberate manual off.** The default rule turns AC output back on within a poll whenever
  grid is present — ideal for an always-on internet UPS, but it means you can't leave AC
  output off by hand while grid is up. Use `--require-recovery` if you want it to act only
  after seeing an outage (note: a wall-powered Pi reboots on grid return and forgets it saw
  the outage, so the default rule is usually what you actually want here).
- **Safety back-off.** If a re-arm doesn't "stick" several times in a row (e.g. the unit
  faults the output back off), the daemon backs off for 30 min and logs an error instead of
  hammering it.
- **No SoC gate needed.** It only re-arms when grid is present, so AC output runs from grid
  pass-through — safe even at low battery.

## Residual risks

- **BLE flakiness / firmware hang.** The AC200L can occasionally hang where buttons and
  Bluetooth both stop responding until a power cycle; no software can fix that. If you ever
  see the daemon unable to connect for a long time with the unit clearly on, that's the case.
- **Multi-day outage.** Covered: the unit auto-wakes and recharges when grid returns no
  matter how long it was dead; the daemon then re-arms. The only true gap is the rare
  firmware hang above.
- **Future encrypted firmware.** Current AC200L firmware is unencrypted. If a future update
  switches it to Bluetti's encrypted BLE protocol, telemetry would stop; we'd need the
  encrypted-handshake path then.

## Optional next add-on (not required to work)

Push alerts on outage/recovery (e.g. ntfy/Pushover) once the network is back after re-arm.
Easy to bolt on; intentionally left out of the core so the re-arm has zero network dependency.
