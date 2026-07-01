/*
 * AC200L auto-restart controller — ESP32 firmware (NimBLE-Arduino).
 *
 * Standalone, network-independent re-arm. Watches one specific Bluetti AC200L over Bluetooth
 * LE and re-enables AC output whenever it is off and it's safe to do so — the state the unit
 * latches into after a full-drain outage. "Safe" = the unit is drawing input power (grid
 * confirmed, e.g. charging right after grid returns) OR SoC is above REARM_SOC_FLOOR (a
 * full/healthy battery draws ~0 W from a connected grid, so zero input power is NOT an outage).
 * The unit auto-wakes and recharges itself when grid returns; this provides the one nudge it
 * won't do on its own: turning AC output on.
 *
 * Target device (REQUIRED, security)
 * ----------------------------------
 * TARGET_DEVICE_ID (in the untracked src/config.h) is the exact Bluetti device ID from the
 * app (== the BLE advertised name). The firmware connects to and re-arms ONLY that unit, by
 * exact-name match — never any other AC200L. If it is empty the firmware re-arms nothing at
 * all (safe no-op): so a mis-flashed or unconfigured board can't flip on someone else's unit.
 *
 * WiFi beacon optimization (OPTIONAL, passwordless)
 * -------------------------------------------------
 * Your WiFi router is powered by the AC200L, so if the router's SSID is on the air, AC output
 * is on and everything is fine. The firmware just SCANS for the configured WIFI_SSID beacon —
 * it never associates, so NO WiFi password is needed or stored (the SSID name isn't secret):
 *   - SSID seen     -> all good. Stay OFF Bluetooth (phone app keeps working) and idle,
 *                      re-checking every WIFI_OK_INTERVAL_MS (2 min). Once every WIFI_HEARTBEAT_MS
 *                      (15 min) do a single BLE read anyway, to prove the link and refresh the
 *                      SoC/power shown on the panel (it reads only; grid is up so it won't re-arm).
 *   - SSID NOT seen -> router likely unpowered (AC output off / dead-latch), and the network
 *                      is down anyway, so poll BLE hard every CHECK_INTERVAL_MS (45 s) for a
 *                      rapid re-arm once grid returns.
 *   - WIFI_SSID empty -> optimization off; poll BLE gently every NO_WIFI_INTERVAL_MS (2 min) so
 *                      it doesn't keep stealing the single BLE slot from someone using the app.
 * NimBLE is initialized once and left up; we connect/disconnect per check but never deinit
 * (deinit-every-cycle deadlocks the NimBLE host task). WiFi is only ever a brief passive scan.
 *
 * Power this board from a GRID wall outlet (USB), NOT from the AC200L, and place it within
 * Bluetooth (and WiFi) range of the unit.
 *
 * OLED status (optional, two-colour panel)
 * ----------------------------------------
 * If an SSD1306 128x64 panel is present (onboard on the ideaspark ESP32-WROOM-32, I2C @ 0x3C,
 * SDA=21/SCL=22) it shows a live dashboard. The panel's top ~16px are physically YELLOW and are
 * used as an attention strip:
 *   - Normal: yellow text on black showing the current status (OK - Armed / Scanning / OUTAGE /
 *     RE-ARMING / WAITING / BACK-OFF / WiFi OK / No device ID set).
 *   - LATCHED alert: the instant an auto re-arm fires, the strip flips to BLACK-ON-YELLOW and
 *     shows "TRIGGERED" at the right. This latch is NEVER cleared in software — it stays until
 *     you physically press reset/reboot, so an intervention is never missed after the fact.
 * The blue zone shows big SoC%, the AC-in W / AC-out on-off line, a WiFi-detected indicator
 * (ok/--/off) with a countdown to the next check, and the full TARGET_DEVICE_ID for verification.
 * Purely informational: if no panel is found the firmware runs headless, logic unaffected.
 *
 * Protocol (proven on real AC200L hardware): Write char 0000ff02-..., Notify char 0000ff01-...
 * (service searched, not hardcoded). Modbus over GATT. Read range 36..49 (func 0x03):
 * 37 ac_input_power(W), 38 ac_output_power(W), 43 SoC(%), 48 ac_output_on(bool).
 * Re-arm: func 0x06 write reg 3007 = 1.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <NimBLEDevice.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---- Config ----------------------------------------------------------------
// Device-specific config lives in an untracked src/config.h so nothing personal is committed.
// Copy src/config.h.example -> src/config.h and fill it in (config.h is gitignored).
//
//   TARGET_DEVICE_ID  REQUIRED. Exact Bluetti device ID as shown in the BLUETTI app (== the
//                     BLE advertised name, e.g. "AC200L2439001209551"). The firmware connects
//                     to and re-arms ONLY this unit. Left empty => re-arm disabled entirely
//                     (safe no-op). This is a security guard: never flip on a neighbour's unit
//                     or a second battery you didn't mean to.
//   WIFI_SSID         OPTIONAL. Home WiFi network NAME only (no password — the firmware just
//                     listens for the router's beacon to tell if it's powered). When set and
//                     the SSID is seen on the air => router up => skip re-arm, check every
//                     15 min. Empty => always check over BLE (every 5 min).
#if defined(__has_include)
#  if __has_include("config.h")
#    include "config.h"
#  endif
#endif
#ifndef TARGET_DEVICE_ID
#  define TARGET_DEVICE_ID ""
#endif
#ifndef WIFI_SSID
#  define WIFI_SSID ""
#endif

// Cadences. BLE is only ever used when the SSID is absent, so it never fights the phone app
// during normal (network-up) use. Two BLE speeds: fast when the SSID is *configured* but not
// seen (a real outage/dead-latch — the network is down anyway, so poll hard for a rapid re-arm),
// and gentle when no SSID is configured at all (there we poll continuously and would otherwise
// keep stealing the BLE slot from a user, so back off).
static const uint32_t WIFI_OK_INTERVAL_MS = 2UL * 60 * 1000;  // SSID seen -> idle 2 min (no BLE)
static const uint32_t CHECK_INTERVAL_MS   = 45UL * 1000;      // SSID configured but absent -> 45s
static const uint32_t NO_WIFI_INTERVAL_MS = 2UL * 60 * 1000;  // no SSID configured -> gentle 2 min
// Even when healthy (SSID seen), do ONE BLE read this often to prove the link and refresh the
// SoC/power readings on the panel. Rare enough to barely touch the phone app's BLE slot.
static const uint32_t WIFI_HEARTBEAT_MS   = 15UL * 60 * 1000; // healthy BLE heartbeat -> 15 min
static const uint32_t SCAN_SECONDS    = 6;
static const uint32_t RESP_TIMEOUT_MS = 5000;

static const uint16_t READ_START = 36;
static const uint16_t READ_COUNT = 14;             // regs 36..49
static const uint16_t AC_OUTPUT_CTRL_REG = 3007;

// Re-arm when output is off and EITHER input power is flowing (grid confirmed) OR SoC is at/above
// this floor. The floor covers the case where grid is present but the unit draws ~0 W (full
// battery, nothing to charge), which would otherwise look like an outage. The acIn>0 term still
// covers the low-SoC dead-latch (drained battery charging hard right after grid returns).
static const uint16_t REARM_SOC_FLOOR = 15;   // percent

// Safety back-off: if a re-arm doesn't "stick" repeatedly, stop hammering and warn.
static const int      MAX_FAILED_REARMS = 5;
static const uint32_t BACKOFF_AFTER_FAIL_MS = 1800000UL; // 30 min
static const uint32_t REARM_GRACE_MS = 90000UL;          // leave alone after a success

// ---- OLED status display ---------------------------------------------------
// Onboard SSD1306 on the ideaspark ESP32-WROOM-32 (128x64, I2C @ 0x3C, SDA=21 SCL=22).
// Purely informational: if no panel is present, begin() fails and the controller runs
// headless — the re-arm logic never depends on the display. Set OLED_ENABLED 0 to build
// for a bare board (drops the Adafruit deps' code paths at compile time).
#define OLED_ENABLED 1
static const uint8_t  OLED_SDA_PIN = 21;
static const uint8_t  OLED_SCL_PIN = 22;
static const uint8_t  OLED_ADDR    = 0x3C;
static const int16_t  OLED_W = 128, OLED_H = 64;

// This is a two-colour panel: the top OLED_YELLOW_H rows are physically YELLOW, the rest blue.
// We use the yellow strip as an "attention" area (see oledDraw / the TRIGGERED latch below).
static const int16_t OLED_YELLOW_H = 16;

// Last-known values shown on the panel (updated from BLE reads / lifecycle events).
static bool     g_haveState = false;
static uint16_t g_dSoc = 0, g_dAcIn = 0, g_dAcOutW = 0;
static bool     g_dAcOn = false;
static char     g_dStatus[22] = "Booting";
// TRIGGERED latch: set true the moment we fire an auto re-arm, and NEVER cleared in software —
// it persists until the board is physically reset/rebooted. This is a deliberate "did the
// controller have to intervene?" flag so a re-arm event isn't missed after the fact.
static bool     g_triggered     = false;
static int8_t   g_wifiSeen      = -1;   // -1 = WiFi disabled, 0 = SSID not seen, 1 = SSID seen
static uint32_t g_nextCheckSecs = 0;    // seconds until the next check, for the OLED countdown
static uint32_t g_lastHeartbeat = 0;    // millis() of the last healthy-state BLE read (0 = never)

#if OLED_ENABLED
static Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
static bool g_oledOk = false;

static void oledDraw() {
  if (!g_oledOk) return;
  oled.clearDisplay();

  // --- Yellow attention strip: current status. Normally yellow-on-black; once a re-arm has
  //     fired it latches to black-on-yellow with "TRIGGERED" at the right until reset.
  if (g_triggered) {
    oled.fillRect(0, 0, OLED_W, OLED_YELLOW_H, SSD1306_WHITE);   // yellow background
    oled.setTextColor(SSD1306_BLACK);
  } else {
    oled.setTextColor(SSD1306_WHITE);                           // yellow text on black
  }
  oled.setTextSize(1);
  oled.setCursor(2, 4);
  oled.print(g_dStatus);
  if (g_triggered) {
    static const char* kTrig = "TRIGGERED";
    oled.setCursor(OLED_W - (int16_t)strlen(kTrig) * 6, 4);
    oled.print(kTrig);
  }

  // --- Blue zone: telemetry, WiFi/countdown, and the full device ID.
  oled.setTextColor(SSD1306_WHITE);
  if (g_haveState) {
    oled.setTextSize(2);
    oled.setCursor(0, 19);            // shifted down one row so it clears the yellow strip
    oled.printf("SoC %u%%", g_dSoc);
    oled.setTextSize(1);
    oled.setCursor(0, 37);
    oled.printf("IN:%uW OUT:%s", g_dAcIn, g_dAcOn ? "ON" : "OFF");
  } else {
    oled.setTextSize(1);
    oled.setCursor(0, 26);
    oled.print(F("(no telemetry yet)"));
  }

  // WiFi detected? + countdown to the next check (always shown; "checking" while a check runs).
  oled.setTextSize(1);
  oled.setCursor(0, 46);
  oled.printf("WiFi:%s", g_wifiSeen < 0 ? "off" : (g_wifiSeen ? "ok" : "--"));
  oled.setCursor(60, 46);           // fixed column, kept off the right edge so it's never clipped
  if (g_nextCheckSecs) {
    oled.printf("in %lu:%02lu",
                (unsigned long)(g_nextCheckSecs / 60), (unsigned long)(g_nextCheckSecs % 60));
  } else {
    oled.print("checking");
  }

  // Full target device ID along the bottom, for at-a-glance verification.
  oled.setCursor(0, 56);
  oled.print(strlen(TARGET_DEVICE_ID) ? TARGET_DEVICE_ID : "(no device ID)");

  oled.display();
}

static void oledInit() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  g_oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!g_oledOk) { Serial.println("OLED not found; running headless."); return; }
  oledDraw();
}

// Set the attention-strip status text and repaint (other globals are drawn as-is).
static void oledStatus(const char* s) {
  strncpy(g_dStatus, s, sizeof(g_dStatus) - 1);
  g_dStatus[sizeof(g_dStatus) - 1] = '\0';
  oledDraw();
}

// Clear the whole panel to black (used to make a refresh visibly obvious).
static void oledBlank() {
  if (!g_oledOk) return;
  oled.clearDisplay();
  oled.display();
}
#else
static void oledInit() {}
static void oledDraw() {}
static void oledStatus(const char*) {}
static void oledBlank() {}
#endif

// Sleep for ms while refreshing the OLED once a second, so the countdown stays live. When the
// countdown hits zero, blank the panel for ~1s before returning so the user sees a clear screen
// refresh on every check — even when the new readings are identical to the previous ones.
static void sleepWithCountdown(uint32_t ms) {
  for (uint32_t r = ms / 1000; r > 0; r--) {
    g_nextCheckSecs = r;
    oledDraw();
    delay(1000);
  }
  g_nextCheckSecs = 0;
  oledBlank();
  delay(1000);
}

// ---- BLE state -------------------------------------------------------------
static NimBLEClient*               g_client     = nullptr;
static NimBLERemoteCharacteristic* g_writeChar  = nullptr;
static NimBLERemoteCharacteristic* g_notifyChar = nullptr;

static uint8_t   g_resp[64];
static volatile size_t g_respLen = 0;
static size_t    g_expectedLen = 0;
static SemaphoreHandle_t g_respSem = nullptr;

static int       g_failedRearms = 0;
static uint32_t  g_graceUntil   = 0;  // millis() value

// ---- Modbus helpers --------------------------------------------------------
static uint16_t modbusCrc(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xA001 : crc >> 1;
  }
  return crc;
}

static void buildFrame(uint8_t func, uint16_t a, uint16_t b, uint8_t* out) {
  out[0] = 0x01; out[1] = func;
  out[2] = a >> 8; out[3] = a & 0xFF;
  out[4] = b >> 8; out[5] = b & 0xFF;
  uint16_t crc = modbusCrc(out, 6);
  out[6] = crc & 0xFF; out[7] = crc >> 8;   // CRC little-endian
}

static uint16_t regAt(uint16_t addr) {
  size_t off = 3 + (addr - READ_START) * 2;   // skip [addr][func][bytecount]
  return ((uint16_t)g_resp[off] << 8) | g_resp[off + 1];
}

// ---- Notification handling -------------------------------------------------
static void notifyCB(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
  for (size_t i = 0; i < len && g_respLen < sizeof(g_resp); i++) g_resp[g_respLen++] = data[i];
  if (g_expectedLen && g_respLen >= g_expectedLen) xSemaphoreGive(g_respSem);
}

static bool transact(const uint8_t* frame, size_t flen, size_t expected) {
  if (!g_client || !g_client->isConnected() || !g_writeChar) return false;
  g_respLen = 0;
  g_expectedLen = expected;
  xSemaphoreTake(g_respSem, 0);  // drain any stale signal
  if (!g_writeChar->writeValue((uint8_t*)frame, flen, false)) return false;
  return xSemaphoreTake(g_respSem, pdMS_TO_TICKS(RESP_TIMEOUT_MS)) == pdTRUE;
}

// ---- High-level ops --------------------------------------------------------
struct State { uint16_t soc; uint16_t acIn; uint16_t acOutW; bool acOutputOn; };

static bool readState(State& s) {
  uint8_t f[8];
  buildFrame(0x03, READ_START, READ_COUNT, f);
  if (!transact(f, 8, 2 * READ_COUNT + 5)) return false;
  if (g_resp[1] & 0x80) return false;
  if (g_respLen != 2 * READ_COUNT + 5) return false;
  uint16_t crc = g_resp[g_respLen - 2] | (g_resp[g_respLen - 1] << 8);
  if (crc != modbusCrc(g_resp, g_respLen - 2)) return false;
  s.soc = regAt(43);
  s.acIn = regAt(37);
  s.acOutW = regAt(38);
  s.acOutputOn = regAt(48) & 1;
  return true;
}

static bool setAcOutput(bool on) {
  uint8_t f[8];
  buildFrame(0x06, AC_OUTPUT_CTRL_REG, on ? 1 : 0, f);
  return transact(f, 8, 8);   // func 0x06 echoes the 8-byte request
}

// ---- BLE connection lifecycle ---------------------------------------------
static bool findChars() {
  g_writeChar = nullptr; g_notifyChar = nullptr;
  std::vector<NimBLERemoteService*>* services = g_client->getServices(true);
  NimBLEUUID uWrite("ff02"), uNotify("ff01");
  for (auto* svc : *services) {
    std::vector<NimBLERemoteCharacteristic*>* chars = svc->getCharacteristics(true);
    for (auto* ch : *chars) {
      if (ch->getUUID() == uWrite)  g_writeChar = ch;
      if (ch->getUUID() == uNotify) g_notifyChar = ch;
    }
  }
  if (!g_writeChar || !g_notifyChar) { Serial.println("  ff01/ff02 characteristics not found"); return false; }
  if (!g_notifyChar->canNotify() || !g_notifyChar->subscribe(true, notifyCB)) {
    Serial.println("  failed to subscribe to notifications"); return false;
  }
  return true;
}

static bool connectToUnit() {
  // Security: connect ONLY to the exact configured device ID, never a prefix/any-AC200L.
  Serial.printf("Scanning %us for '%s'...\n", SCAN_SECONDS, TARGET_DEVICE_ID);
  oledStatus("Scanning...");
  NimBLEScan* scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults found = scan->start(SCAN_SECONDS, false);

  for (int i = 0; i < found.getCount(); i++) {
    NimBLEAdvertisedDevice d = found.getDevice(i);   // returned by value in NimBLE 1.4
    if (d.getName() != TARGET_DEVICE_ID) continue;   // exact match only
    Serial.printf("Found %s [%s]; connecting...\n", d.getName().c_str(),
                  d.getAddress().toString().c_str());
    if (!g_client) g_client = NimBLEDevice::createClient();   // reuse one client for all cycles
    g_client->setConnectTimeout(10);
    if (!g_client->connect(d.getAddress())) {
      Serial.println("  connect failed");
      scan->clearResults();
      return false;
    }
    scan->clearResults();
    if (!findChars()) { g_client->disconnect(); return false; }
    Serial.println("Connected and subscribed.");
    return true;
  }
  scan->clearResults();
  Serial.println("  target device not found (on? in range? correct ID? phone app closed?)");
  oledStatus("Unit not found");
  return false;
}

// One full BLE session: connect, read, re-arm if needed, disconnect. NimBLE is initialized
// once in setup() and stays up; we reuse a single client and only connect/disconnect per cycle
// (no per-loop deinit — that deadlocks). Being initialized-but-disconnected does NOT hold the
// unit's single BLE slot, so the phone app is free between checks.
static void bleCheckAndRearm() {
  if (!connectToUnit()) return;

  State s;
  if (!readState(s)) { Serial.println("read failed"); oledStatus("BLE read failed"); g_client->disconnect(); return; }

  // "Safe to re-arm" = output is off AND either the unit is drawing input power (grid confirmed,
  // e.g. the post-drain dead-latch while charging) OR SoC is above a floor (battery full/healthy,
  // so re-arming is safe even when input power reads 0 — a full battery with grid connected draws
  // ~0 W, which is NOT an outage). Using input *power* alone as the grid proxy misfires here.
  bool charging = s.acIn > 0;
  bool safeToArm = charging || (s.soc >= REARM_SOC_FLOOR);
  Serial.printf("SoC %u%% | AC-in %uW (%s) | AC-out %uW (%s)\n",
                s.soc, s.acIn, charging ? "charging" : "idle-in",
                s.acOutW, s.acOutputOn ? "on" : "OFF");

  // Push fresh telemetry to the panel (status line set per-branch below).
  g_haveState = true;
  g_dSoc = s.soc; g_dAcIn = s.acIn; g_dAcOutW = s.acOutW; g_dAcOn = s.acOutputOn;

  uint32_t now = millis();
  bool need = !s.acOutputOn && safeToArm;

  if (need && (int32_t)(g_graceUntil - now) > 0) {
    Serial.println("re-arm condition met but within grace/back-off; waiting");
    oledStatus("WAITING (grace)");
  } else if (need && g_failedRearms >= MAX_FAILED_REARMS) {
    Serial.printf("re-arm failed %d times; backing off %lus\n",
                  g_failedRearms, BACKOFF_AFTER_FAIL_MS / 1000);
    g_graceUntil = now + BACKOFF_AFTER_FAIL_MS;
    g_failedRearms = 0;
    oledStatus("BACK-OFF (failing)");
  } else if (need) {
    Serial.printf("AC output OFF + safe (%s) -> re-arming AC output\n",
                  charging ? "grid charging" : "SoC above floor");
    g_triggered = true;                 // latch: the controller had to intervene (until reset)
    oledStatus("RE-ARMING...");
    if (setAcOutput(true)) {
      // The AC200L's output relay can take several seconds to engage and report, so poll the
      // read-back (not a single check) — confirm in-session rather than falsely reporting failure
      // and waiting for the next poll cycle. Exit as soon as the unit reports output on.
      State after;
      bool armed = false;
      for (int i = 0; i < 6 && !armed; i++) {
        delay(1500);
        if (readState(after) && after.acOutputOn) armed = true;
      }
      if (armed) {
        Serial.println("re-arm SUCCESS (AC output now ON)");
        g_failedRearms = 0;
        g_graceUntil = now + REARM_GRACE_MS;
        // Refresh the panel from the post-re-arm read-back so IN/SoC/OUT are all current.
        g_dSoc = after.soc; g_dAcIn = after.acIn; g_dAcOutW = after.acOutW; g_dAcOn = after.acOutputOn;
        oledStatus("RE-ARMED");
      } else {
        g_failedRearms++;
        Serial.printf("re-arm not confirmed within ~9s (attempt %d); will recheck next poll\n", g_failedRearms);
        oledStatus("re-arm pending");
      }
    } else {
      g_failedRearms++;
      Serial.printf("re-arm write failed (attempt %d)\n", g_failedRearms);
      oledStatus("re-arm wr failed");
    }
  } else if (!s.acOutputOn) {
    // Output off but not safe to arm: no input power and SoC below the floor (a genuine
    // low-battery outage). Wait for grid to return / the unit to start charging.
    Serial.println("AC output OFF but SoC below floor and no grid -> waiting");
    oledStatus("LOW SoC - waiting");
  } else {  // output on
    g_failedRearms = 0;  // healthy
    oledStatus("OK - Armed");
  }

  if (g_client && g_client->isConnected()) {
    g_client->disconnect();
    delay(200);   // let the disconnect complete before the radio goes idle / WiFi scans
  }
}

// ---- WiFi beacon probe -----------------------------------------------------
// Passwordless: scan the air and report whether the configured SSID's beacon is present.
// The router is powered by the AC200L, so "SSID on the air" => router booted => AC output on.
// No association, no password — only the (non-secret) SSID name is used. Returns false when
// the optimization is disabled (no SSID), so the caller always falls through to the BLE check.
static bool ssidBeaconPresent() {
  if (strlen(WIFI_SSID) == 0) { g_wifiSeen = -1; return false; }
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, true);                 // ensure we only listen, never associate
  int n = WiFi.scanNetworks(false, false);      // blocking scan, hidden APs excluded
  bool found = false;
  for (int i = 0; i < n; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) { found = true; break; }
  }
  WiFi.scanDelete();
  WiFi.mode(WIFI_OFF);
  delay(100);
  g_wifiSeen = found ? 1 : 0;
  return found;
}

// ---- Arduino entry points --------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\nAC200L auto-restart controller (ESP32)");
  if (strlen(TARGET_DEVICE_ID) == 0)
    Serial.println("No TARGET_DEVICE_ID set -> re-arm DISABLED (safe no-op).");
  else
    Serial.printf("Target device: %s\n", TARGET_DEVICE_ID);
  if (strlen(WIFI_SSID) == 0)
    Serial.println("No WIFI_SSID set -> always check over BLE (every 2 min).");
  else
    Serial.printf("WiFi beacon optimization on for SSID '%s' (idle 2 min seen; 45s BLE when down).\n", WIFI_SSID);
  oledInit();
  g_respSem = xSemaphoreCreateBinary();
  // Initialize NimBLE once and leave it up for the life of the program. We connect/disconnect
  // per check but never deinit — deinit(true) every cycle deadlocks the host task.
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
}

void loop() {
  // Security guard: with no target device configured we never touch any unit.
  if (strlen(TARGET_DEVICE_ID) == 0) {
    Serial.println("No target device ID configured; not re-arming.");
    oledStatus("No device ID set");
    sleepWithCountdown(NO_WIFI_INTERVAL_MS);
    return;
  }

  // WiFi beacon shortcut: SSID on the air => router up => AC output on => nothing to re-arm.
  if (ssidBeaconPresent()) {
    uint32_t now = millis();
    // Periodic heartbeat: even while healthy, do one BLE read every WIFI_HEARTBEAT_MS (and once
    // right after boot) to prove the link still works and refresh SoC/power on the panel. Grid is
    // up and output is on, so bleCheckAndRearm just reads telemetry — it won't actually re-arm.
    if (g_lastHeartbeat == 0 || (now - g_lastHeartbeat) >= WIFI_HEARTBEAT_MS) {
      Serial.println("SSID present -> healthy; BLE heartbeat (read + display).");
      oledStatus("WiFi OK - heartbeat");
      bleCheckAndRearm();
      g_lastHeartbeat = millis();
    } else {
      Serial.println("SSID present -> router powered / AC output on. Idle 2 min (no re-arm).");
      oledStatus("WiFi OK - router up");
    }
    sleepWithCountdown(WIFI_OK_INTERVAL_MS);
    return;
  }

  // SSID not seen (or WiFi disabled): check the unit over BLE and re-arm if needed.
  Serial.println("SSID not seen -> checking the AC200L over Bluetooth.");
  oledStatus("Checking BLE...");
  bleCheckAndRearm();

  // Fast poll during a real outage (SSID configured but down); gentle when no SSID is set at all
  // so we don't keep stealing the BLE slot from someone using the phone app.
  sleepWithCountdown(strlen(WIFI_SSID) == 0 ? NO_WIFI_INTERVAL_MS : CHECK_INTERVAL_MS);
}
