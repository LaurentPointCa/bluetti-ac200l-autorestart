# AC200L auto-restart controller — ESP32 firmware

Standalone NimBLE firmware that re-enables the AC200L's AC output whenever grid power is
present but output is off (the state it latches into after a full-drain outage). No network,
no Home Assistant, no cloud — it talks Bluetooth directly to the unit and does everything
on-device. This is the appliance version of the Python controller; same proven protocol.

## Why ESP32 here

- Instant boot (boot == grid returned), no SD card to corrupt, very low power, ~$6 board.
- Fully self-contained: the re-arm has zero network dependency, which matters because the
  AC200L powers the home internet gear (no LAN/cloud until the unit is re-armed).

## Hardware to buy

- A **classic ESP32 DevKit board** (the `esp32dev` target). Any with BLE works: classic
  ESP32, ESP32-C3, or ESP32-S3. **Do NOT buy ESP32-S2 — it has no Bluetooth.**
  - Recommended, easiest: a generic "ESP32 DevKitC" / "ESP32 WROOM-32" dev board.
  - Deployed board: **ideaspark ESP32-WROOM-32 with onboard 0.96" SSD1306 OLED** (classic
    ESP32, `esp32dev` target, WiFi+BLE, CH340 USB-serial over micro-USB). The OLED is optional
    (see below) — the firmware runs headless on any plain ESP32.
  - For C3 or S3, uncomment the matching env in `platformio.ini`.
- A **data** USB cable (many micro-USB cables are charge-only and won't enumerate) + a **wall
  USB power supply on grid power** (NOT the AC200L).
- Place it within Bluetooth range of the unit (same room).

### Driver note (CH340 / micro-USB boards)

The ideaspark board uses a **CH340** USB-serial chip (not CP210x). Recent macOS ships an in-box
CH34x driver, so it usually appears as `/dev/cu.wchusbserial*` or `/dev/cu.usbserial*` with no
install. If the board doesn't enumerate at all (nothing new under `ls /dev/cu.*`, nothing in
`ioreg -p IOUSB`), it's almost always a **charge-only cable** or a passive hub/adaptor — swap to
a known-good data cable plugged **directly** into the Mac before installing any driver. If it
enumerates in `ioreg` but no `/dev/cu.*` node appears, then install WCH's CH34x macOS driver.

## Build & flash (PlatformIO)

1. Install VS Code + the **PlatformIO IDE** extension, or the PlatformIO Core CLI. A local
   CLI venv is already set up here: `.venv-pio/bin/pio` (created with
   `python3 -m venv .venv-pio && .venv-pio/bin/pip install platformio`).
2. Open this `esp32/` folder in VS Code (PlatformIO detects `platformio.ini`).
3. Plug in the board over USB (data cable).
4. Click **Upload** (PlatformIO toolbar), or from the CLI in this folder:
   ```sh
   .venv-pio/bin/pio run -e esp32dev --target upload   # auto-detects the /dev/cu.* port
   .venv-pio/bin/pio device monitor -b 115200          # watch the serial log
   ```
   PlatformIO auto-installs the ESP32 toolchain and the NimBLE-Arduino library on first build.

## Testing

First set `TARGET_DEVICE_ID` in `src/config.h` (see Configuration below). With the **BLUETTI
phone app closed** (the unit allows only one BLE connection), watch the serial monitor. Each
BLE check (every 5 min, or 15 min while the WiFi SSID is seen) you should see:
```
SoC 87% | AC-in 142W (grid) | AC-out 118W (on)
```
To verify the re-arm: turn AC output off (phone app, or the Mac write PoC), and within a poll
the log should show:
```
grid present + AC output OFF -> re-arming AC output
re-arm SUCCESS (AC output now ON)
```
That exactly reproduces the post-outage recovery. For a faster test loop, temporarily lower
`CHECK_INTERVAL_MS` near the top of `src/main.cpp`.

## OLED status display (optional)

If an **SSD1306 128×64** panel is on I2C `0x3C` (onboard on the ideaspark board, `SDA=21`,
`SCL=22`) the firmware shows live status; otherwise it runs headless and the re-arm logic is
unaffected. The screen shows big **SoC %**, the **AC-in W** and **AC-out on/off** line, and a
bottom state bar that reads one of:

- `Scanning...` / `Unit not found` — looking for the AC200L over BLE.
- `OK - armed` — grid present, AC output on (healthy).
- `OUTAGE (no grid)` — no grid input seen (unit on battery).
- `RE-ARMING...` → `RE-ARMED` — the recovery nudge in progress / succeeded.
- `WAITING (grace)` / `BACK-OFF (failing)` — post-success grace or repeated-failure back-off.
- `WiFi OK - router up` — SSID beacon seen ⇒ router powered ⇒ all good (idle 15 min).
- `No device ID set` — `TARGET_DEVICE_ID` is empty, so re-arm is disabled (safe no-op).

Pins/address are set near the top of `src/main.cpp`. To build for a bare board without the
display, set `#define OLED_ENABLED 0` there (drops the Adafruit dependencies from the build).

## Configuration (required)

Config lives in an **untracked** `src/config.h` so nothing personal is committed:

```sh
cp src/config.h.example src/config.h   # then edit src/config.h
```

It defines two things (`main.cpp` includes the file only if present, via `__has_include`):

- **`TARGET_DEVICE_ID`** — **required.** The exact Bluetti device ID shown in the BLUETTI app
  (it's the BLE advertised name, e.g. `AC200L2439001209551`). The ESP32 connects to and
  re-arms **only** this unit, by exact-name match. **Leave it empty and re-arm is disabled
  entirely** (the OLED shows `No device ID set`). This is a deliberate **security guard**: an
  unconfigured or mis-flashed board can never flip on a neighbour's unit or a second battery.
- **`WIFI_SSID`** — optional; see below. **No password** is ever needed or stored.

## WiFi optimization (optional, passwordless)

Set `WIFI_SSID` in `config.h` to your home network **name** (SSID only — no password). 

How it works: **your router is powered by the AC200L**, so if its SSID is on the air, the
router booted, which means AC output is on and everything is fine. The firmware simply **scans
for the SSID beacon** — it never associates, so there's no password to store or leak (the SSID
name isn't a secret). When the SSID is seen, the firmware stays **completely off Bluetooth** and
idles, re-checking every **15 min**. When the SSID is *not* seen (router unpowered → AC output
off / dead-latch), it switches on Bluetooth, checks the unit, and re-arms.

Two benefits:
1. During normal operation the ESP32 isn't holding the unit's Bluetooth slot, so **your phone
   Bluetti app works normally**. It only grabs BLE during an actual recovery.
2. Less radio activity / power.

Requirements & assumptions:
- The ESP32 must be within **WiFi range** as well as Bluetooth range.
- Assumes the **router is on the AC200L**. If your router is on separate/grid power, leave
  `WIFI_SSID` empty — then the firmware checks over BLE every **5 min** (still correct; it grabs
  the BLE slot briefly each check, so close the phone app if it clashes).
- WiFi and BLE are used **sequentially** (never both radios at once) to avoid ESP32
  coexistence problems.

Leave `WIFI_SSID` empty to disable this and check over BLE every 5 minutes.

## Tuning (top of `src/main.cpp`)

- `WIFI_OK_INTERVAL_MS` — idle time when the SSID is seen (default 15 min).
- `CHECK_INTERVAL_MS` — BLE re-check cadence otherwise (default 5 min).
- `TARGET_DEVICE_ID` / `WIFI_SSID` — set these in `src/config.h`, not here.
- Safety back-off: `MAX_FAILED_REARMS`, `BACKOFF_AFTER_FAIL_MS`, `REARM_GRACE_MS`.

## Notes & residual risks

- **Single BLE connection:** the unit allows only one BLE connection at a time. With the WiFi
  optimization enabled, the ESP32 is normally OFF Bluetooth, so the phone app works as usual;
  it only takes the BLE slot briefly during a recovery. With WiFi disabled it connects for a
  moment every 5 min (then disconnects) — close the phone app if the two clash.
- **Service UUID:** the firmware searches all services for the `ff01`/`ff02` characteristics
  rather than assuming `ff00` vs `fff0`, so it works regardless of which the unit uses.
- **Firmware hang:** the rare AC200L state where buttons AND Bluetooth freeze until a manual
  power cycle can't be fixed by any controller. If the serial log shows it can never connect
  while the unit is clearly on, that's the case.
- **Multi-day outage:** covered — the unit auto-wakes and recharges whenever grid returns,
  then this re-arms it.
- **Future encrypted firmware:** current AC200L firmware is unencrypted. A future update to
  Bluetti's encrypted BLE protocol would stop telemetry and require the encrypted handshake.

## Optional later

Add Wi-Fi + an HTTP/MQTT/ntfy push on outage/recovery once the network is back. The ESP32 has
Wi-Fi; keep it strictly optional so the re-arm itself never depends on the network.
