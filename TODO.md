# Bluetti AC200L Auto-Restart Project

## Problem
AC200L in UPS mode: if grid fails and the unit drains to 0%, when grid returns the
unit does NOT re-enable AC output on its own. Bluetti declined to fix this in firmware
(cites "safety risk"). No native setting exists.

## Decisions (2026-06-29)
- Goal: re-arm the **Bluetti itself** so it's ready again (not just recover loads).
- Hardware: small additions OK (Raspberry Pi + code). **No ATS.**
- Cannot easily run the physical drain test.

## Findings that shaped the plan
- DEAD END: native Custom UPS SoC floor does NOT help. App manual: SoC Low "When connected
  to the grid, power supply stops below 20%. If no grid is available, it powers appliances
  until 0%." The floor is ignored during a real outage → unit still drains to 0. Drop the
  native-config fix.
- PIVOTAL (favorable): the unit AUTO-WAKES from 0% when grid returns — it auto-charges and
  becomes remotely controllable again ("won't auto-resume AC output... must restart manually
  OR use the mobile app", per Bluetti for the AC200Max). No physical button needed.

## Chosen approach — Pi re-arm, RELAXED scenario
Because the unit revives itself and is controllable once grid is back, the Pi does NOT need
to race to catch the unit before 0%. It waits until grid returns + unit charging, then sends
AC-output-ON, retrying as long as needed (tolerates flaky BLE; no deadline). This directly
gives "Bluetti always ready."

Pi (always-on, grid-powered so it's alive when grid returns) using **ftrueck's fork / upstream
PR #106** (NOT the broken dej7 fork). Roles:
- Detect grid return (ac_input_power reg 37 goes nonzero) + SoC (reg 43) above safe level.
- Re-arm: write ac_output_on = register 3007.
- Observability/logging + push alerts on outage/recovery.
- Alternative control paths to evaluate vs flaky local BLE: (b) UI-automate the official
  Bluetti app on an emulator/old phone (proven by an owner), (c) Bluetti WiFi cloud API.

## Open questions / risks
- [x] Writable AC-output command exists? YES — reg 3007 (writable range 3000-3062),
      mirrored from AC200M. But only anecdotally confirmed working; verify on our firmware.
- [x] Native SoC floor as outage reserve? NO — ignored during outages (drains to 0).
- [x] Auto-wake from 0%? YES (per Bluetti for AC200Max) — auto-charges + app-controllable.
      Confirm for AC200L specifically.
- [ ] dej7 fork is BROKEN (missing ac200l.py). Use ftrueck fork / PR #106 instead.
- [ ] BLE reliability: multiple AC200L owners report it won't even connect (upstream #104).
      Mitigated by the no-deadline relaxed scenario, but still the main technical risk.
- [ ] Control path choice: local BLE (bluetti_mqtt) vs official-app UI automation vs WiFi
      cloud API. Pick the most reliable for AC200L.
- [ ] BLE single-connection: Pi connected => phone app can't connect. Mitigation TBD.
- [ ] Confirm grid-powered Pi is the right power source (alive when grid returns = yes).

## BLE state of play (2025-2026) — favorable
- AC200L is NOT encrypted on current firmware. Working local-BLE tools exist TODAY:
  Patrick762/bluetti_bt (+ bluetti-bt-lib) and ftrueck/bluetti_mqtt — both do read +
  AC-output toggle (ctrl_ac). Old "can't connect" = a name-parsing bug, fixed in forks.
- GATT: FFF0 service; notify ff01, write ff02; Modbus frames; no pairing for AC200L.
- macOS dev works (bleak) but address = CoreBluetooth UUID not MAC — scan & connect by UUID.

## Hardware answer
- DEBUG/validate NOW: Laurent's Mac (built-in BLE, runs bleak). No purchase needed.
- DEPLOY: a Pi with built-in BLE (Zero 2 W suffices), powered from a GRID wall outlet, in
  BLE range of the unit. Talks BLE DIRECTLY — no LAN dependency.
- DESIGN TRAP (avoid): NOT Home Assistant + ESP32 BLE-proxy — the proxy needs the LAN, but
  the router is on the AC200L and is down until re-arm. Chicken-and-egg. Self-contained only.

## Network-independence is the governing constraint
The battery powers the internet gear, so during/after an outage there is NO LAN and NO cloud
until the unit re-arms. Every control path must be LOCAL and self-contained. Rules out
WiFi/cloud AND ESP32-proxy. Leaves: direct-BLE Pi, or a physical button pusher.

## Next steps
1. [DONE + VERIFIED ON HARDWARE 2026-06-29] Mac PoC read-only `poc/ac200l_read.py` works
   perfectly — connects to the real AC200L and reads SoC / AC-in / AC-out. BLE path proven
   on Laurent's actual unit + firmware (unencrypted, Modbus func 0x03, regs 36-49).
2. [DONE + VERIFIED ON HARDWARE 2026-06-29] Write test `poc/ac200l_write.py` — set OFF and
   back ON both confirmed visually + via read-back. The re-arm command (func 0x06 to reg 3007,
   value 1) WORKS on Laurent's AC200L. Core capability fully proven.
3. [DONE - awaiting Mac/Pi test] Controller built:
   - `controller/ac200l_autorestart.py` — daemon. Rule: grid present (ac_in>0) + AC output
     off => re-arm (write reg 3007 ON), read-back confirm. Connect-per-poll (frees BLE),
     caches address, re-scans on failure. Safety back-off after repeated failed re-arms.
     --dry-run, --interval, --require-recovery, --verbose. Py3.9-compatible (future-annotations).
   - `controller/ac200l-autorestart.service` — systemd unit (Restart=always, After=bluetooth).
   - `controller/DEPLOY.md` — Pi Zero 2 W rec, setup, systemd install, behavior/risks.
4. [DONE - awaiting compile/flash] ESP32 firmware port (chosen target: ESP32 + PlatformIO):
   - `esp32/src/main.cpp` — NimBLE standalone controller. Searches services for ff01/ff02
     (robust to ff00 vs fff0), same Modbus framing/registers/re-arm as Python. Safety back-off.
   - `esp32/platformio.ini` — esp32dev default; C3/S3 envs commented.
   - `esp32/README.md` — board to buy (avoid S2), build/flash/test.
   - COMPILE VERIFIED locally via PlatformIO (esp32dev): [SUCCESS], flash 45.8%, RAM 10.9%.
     Fixed NimBLE 1.4 API: getDevice() returns by value; connect by NimBLEAddress.
5. [DONE] WiFi optimization in firmware: if the ESP32 can associate with the home AP, the
   router is powered => AC output is on => idle (stay OFF Bluetooth so the phone app works),
   re-check every 5 min. If WiFi unreachable => bring up BLE, check AC200L, re-arm. WiFi/BLE
   used sequentially. Set WIFI_SSID/PASS in src/main.cpp; empty = continuous BLE polling.
   Assumes the router is powered by the AC200L. Recompiles clean (flash 45.9%).
6. [DONE] Pushed to private GitHub repo:
   https://github.com/LaurentPointCa/bluetti-ac200l-autorestart (main). .pio/.venv/.claude
   gitignored. 12 source/doc files. Top-level README added.
7. [DONE + VERIFIED ON HARDWARE 2026-07-01] ESP32 flashed & running: ideaspark ESP32-WROOM-32,
   micro-USB (CH340), enumerated as /dev/cu.usbserial-1130. NO driver install needed (in-box
   CH34x). Enumeration gotcha: first two micro-USB cables were CHARGE-ONLY (LED lit, no USB
   device in `ioreg -p IOUSB` at all); 3rd cable = data → worked. Flashed WiFi-off bench build.
   Live proof: firmware boots, OLED init OK (no "headless" msg), BLE scan FOUND the real unit
   (AC200L2439001209551, 88:13:bf:37:7f:5a), connected on retry, read telemetry. OLED shows
   SoC 100% / OUT:ON / "OK - armed". Re-arm WRITE not exercised (unit already healthy) but same
   framing as the proven PoC. NOTE: pio device monitor needs a TTY (fails headless); read serial
   via .venv-pio/bin/python + pyserial with an RTS reset pulse instead.
8. [DONE 2026-07-01] OLED status display added to ESP32 firmware. Board: ideaspark
   ESP32-WROOM-32, onboard 0.96" SSD1306 (128x64, I2C @ 0x3C, SDA=21/SCL=22 — confirmed by
   Laurent, matches lifebreath `Wire.begin(21,22)`). Shows big SoC / AC-in W / AC-out on-off /
   state bar (Scanning/OK/OUTAGE/RE-ARMING/RE-ARMED/WAITING/BACK-OFF/WiFi OK). OLED is optional
   (runtime-disabled if begin() fails → headless; compile-time via OLED_ENABLED 0). Added
   Adafruit SSD1306+GFX to platformio.ini. COMPILE VERIFIED (esp32dev): [SUCCESS], flash 48.4%,
   RAM 11.1%. pio CLI reinstalled to esp32/.venv-pio (prior penv was gone). README updated.
9. Optional later: WiFi push alerts (ntfy/Pushover) after network returns.
10. [DONE 2026-07-01] Published to public GitHub + secret/config hygiene. Repo is now PUBLIC:
    github.com/LaurentPointCa/bluetti-ac200l-autorestart. No secrets in history (WiFi always
    empty). Config moved to untracked esp32/src/config.h (gitignored) via __has_include, with a
    committed config.h.example.
11. [DONE + VERIFIED ON HARDWARE 2026-07-01] Reworked WiFi + targeting per Laurent:
    - NO WiFi PASSWORD anywhere. Dropped WPA association; WiFi optimization now a passwordless
      BEACON SCAN (WiFi.scanNetworks, match SSID name only). Rationale: on WPA2-PSK the key is
      always recoverable from flash, so the win is to not need it — SSID beacon presence is the
      same "router powered?" signal. True anti-theft would need eFuse flash encryption (not done).
    - TARGET_DEVICE_ID now MANDATORY + exact-match (was "AC200L" prefix, any unit). Security:
      only ever re-arms the one configured unit; empty => re-arm disabled (safe no-op, OLED
      "No device ID set"). Laurent's unit = AC200L2439001209551.
    - Cadence: SSID seen -> idle 15 min; else BLE check every 5 min (was 45s).
    - Verified on hardware: boots, "Target device: AC200L...", exact-match scan found + connected
      first try, read SoC 100%/AC-in 125W(grid)/AC-out 125W(on), OLED "OK - armed".
12. [DONE + VERIFIED ON HARDWARE 2026-07-01] OLED cosmetic rework (two-colour panel). Top ~16px
    yellow = attention strip: status text yellow-on-black; on a re-arm it LATCHES to black-on-
    yellow + "TRIGGERED" (right side), cleared only by physical reset. SoC line shifted down to
    clear the yellow. Blue zone adds WiFi indicator (ok/--/off), a live per-second countdown to
    next check (sleepWithCountdown replaces blocking delay), and the full TARGET_DEVICE_ID along
    the bottom for verification. Documented in main.cpp header + esp32/README.md. Compiles + flashed;
    read SoC 100%/122W confirms live layout.
3. If solid: buy Pi Zero 2 W, port the script, power from wall, deploy near the unit, automate
   the re-arm + (deferred) alerting.
4. Keep the button pusher as the fallback if BLE proves flaky in practice.
