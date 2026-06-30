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
  - For C3 or S3, uncomment the matching env in `platformio.ini`.
- A USB cable + a **wall USB power supply on grid power** (NOT the AC200L).
- Place it within Bluetooth range of the unit (same room).

## Build & flash (PlatformIO)

1. Install VS Code + the **PlatformIO IDE** extension (or the PlatformIO Core CLI).
2. Open this `esp32/` folder in VS Code (PlatformIO detects `platformio.ini`).
3. Plug in the board over USB.
4. Click **Upload** (PlatformIO toolbar), or from the CLI in this folder:
   ```sh
   pio run -e esp32dev --target upload
   pio device monitor          # watch the serial log at 115200 baud
   ```
   PlatformIO auto-installs the ESP32 toolchain and the NimBLE-Arduino library on first build.

## Testing

With the **BLUETTI phone app closed** (the unit allows only one BLE connection), watch the
serial monitor. You should see, every ~60s:
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
`POLL_INTERVAL_MS` near the top of `src/main.cpp`.

## WiFi optimization (recommended)

Set `WIFI_SSID` / `WIFI_PASS` near the top of `src/main.cpp` to your home network.

How it works: **your WiFi router is powered by the AC200L**, so if the ESP32 can associate
with the AP, the router is up, which means AC output is on and everything is fine. In that
state the firmware stays **completely off Bluetooth** and just idles, re-checking every 5
minutes. Only when WiFi becomes unreachable (router unpowered → AC output off / dead-latch)
does it switch on Bluetooth, check the AC200L, and re-arm.

Two benefits:
1. During normal operation the ESP32 isn't holding the unit's Bluetooth slot, so **your phone
   Bluetti app works normally**. It only grabs BLE during an actual recovery.
2. Less radio activity / power.

Requirements & assumptions:
- The ESP32 must be within **WiFi range** as well as Bluetooth range.
- Assumes the **router is on the AC200L**. If your router is on separate/grid power, leave
  `WIFI_SSID` empty — then the firmware just polls over BLE continuously (still correct, but
  it holds the BLE slot, so close the phone app while it runs).
- WiFi and BLE are used **sequentially** (never both radios at once) to avoid ESP32
  coexistence problems.

Leave `WIFI_SSID = ""` to disable this and fall back to continuous BLE polling.

## Tuning (top of `src/main.cpp`)

- `WIFI_OK_INTERVAL_MS` — idle time when WiFi is up (default 5 min).
- `BLE_RETRY_INTERVAL_MS` — re-check cadence when WiFi is down (default 45s).
- `DEVICE_NAME_PREFIX` — BLE name to match (default `AC200L`).
- Safety back-off: `MAX_FAILED_REARMS`, `BACKOFF_AFTER_FAIL_MS`, `REARM_GRACE_MS`.

## Notes & residual risks

- **Single BLE connection:** the unit allows only one BLE connection at a time. With the WiFi
  optimization enabled, the ESP32 is normally OFF Bluetooth, so the phone app works as usual;
  it only takes the BLE slot briefly during a recovery. With WiFi disabled it holds the slot
  continuously — unplug it if you need the app.
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
