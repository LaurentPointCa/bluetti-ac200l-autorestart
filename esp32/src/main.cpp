/*
 * AC200L auto-restart controller — ESP32 firmware (NimBLE-Arduino).
 *
 * Standalone, network-independent re-arm. Watches one specific Bluetti AC200L over Bluetooth
 * LE and re-enables AC output whenever grid power is present but AC output is off — the state
 * the unit latches into after a full-drain outage. The unit auto-wakes and recharges itself
 * when grid returns; this provides the one nudge it won't do on its own: turning AC output on.
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
 *                      re-checking every WIFI_OK_INTERVAL_MS (15 min).
 *   - SSID NOT seen -> router likely unpowered (AC output off / dead-latch). Switch on
 *                      Bluetooth, check the unit, re-arm if grid is present.
 *   - WIFI_SSID empty -> optimization off; always check over BLE every CHECK_INTERVAL_MS (5 min).
 * WiFi and BLE are used sequentially (never both radios at once) to avoid coexistence issues.
 *
 * Power this board from a GRID wall outlet (USB), NOT from the AC200L, and place it within
 * Bluetooth (and WiFi) range of the unit.
 *
 * OLED status (optional)
 * ----------------------
 * If an SSD1306 128x64 panel is present (onboard on the ideaspark ESP32-WROOM-32, I2C @ 0x3C,
 * SDA=21/SCL=22) it shows live SoC, AC-in W, AC-out on/off, and a state line (Scanning /
 * WAITING / RE-ARMING / RE-ARMED / OK / OUTAGE / WiFi OK). It is purely informational: if no
 * panel is found the firmware runs headless and the re-arm logic is unaffected.
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

static const uint32_t WIFI_OK_INTERVAL_MS = 15UL * 60 * 1000; // SSID seen -> idle 15 min
static const uint32_t CHECK_INTERVAL_MS   = 5UL * 60 * 1000;  // normal BLE check cadence (5 min)
static const uint32_t SCAN_SECONDS    = 6;
static const uint32_t RESP_TIMEOUT_MS = 5000;

static const uint16_t READ_START = 36;
static const uint16_t READ_COUNT = 14;             // regs 36..49
static const uint16_t AC_OUTPUT_CTRL_REG = 3007;

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

// Last-known values shown on the panel (updated from BLE reads / lifecycle events).
static bool     g_haveState = false;
static uint16_t g_dSoc = 0, g_dAcIn = 0, g_dAcOutW = 0;
static bool     g_dAcOn = false;
static char     g_dStatus[22] = "Booting";

#if OLED_ENABLED
static Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
static bool g_oledOk = false;

static void oledDraw() {
  if (!g_oledOk) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(F("AC200L auto-restart"));
  oled.drawFastHLine(0, 10, OLED_W, SSD1306_WHITE);

  if (g_haveState) {
    oled.setTextSize(2);
    oled.setCursor(0, 15);
    oled.printf("SoC %u%%", g_dSoc);
    oled.setTextSize(1);
    oled.setCursor(0, 37);
    oled.printf("IN:%uW  OUT:%s", g_dAcIn, g_dAcOn ? "ON" : "OFF");
  } else {
    oled.setTextSize(1);
    oled.setCursor(0, 22);
    oled.print(F("(no telemetry yet)"));
  }

  // Status line: inverted bar along the bottom for at-a-glance state.
  const int16_t barY = 52;
  oled.fillRect(0, barY, OLED_W, OLED_H - barY, SSD1306_WHITE);
  oled.setTextColor(SSD1306_BLACK);
  oled.setCursor(2, barY + 2);
  oled.print(g_dStatus);
  oled.display();
}

static void oledInit() {
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  g_oledOk = oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (!g_oledOk) { Serial.println("OLED not found; running headless."); return; }
  oledDraw();
}

// Set the bottom status line and repaint (telemetry globals are drawn as-is).
static void oledStatus(const char* s) {
  strncpy(g_dStatus, s, sizeof(g_dStatus) - 1);
  g_dStatus[sizeof(g_dStatus) - 1] = '\0';
  oledDraw();
}
#else
static void oledInit() {}
static void oledStatus(const char*) {}
#endif

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
    g_client = NimBLEDevice::createClient();
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

// One full BLE session: connect, read, re-arm if needed, disconnect. BLE radio must be
// initialized by the caller and is left initialized (deinit handled in loop()).
static void bleCheckAndRearm() {
  if (!connectToUnit()) return;

  State s;
  if (!readState(s)) { Serial.println("read failed"); oledStatus("BLE read failed"); g_client->disconnect(); return; }

  bool grid = s.acIn > 0;
  Serial.printf("SoC %u%% | AC-in %uW (%s) | AC-out %uW (%s)\n",
                s.soc, s.acIn, grid ? "grid" : "OUTAGE",
                s.acOutW, s.acOutputOn ? "on" : "OFF");

  // Push fresh telemetry to the panel (status line set per-branch below).
  g_haveState = true;
  g_dSoc = s.soc; g_dAcIn = s.acIn; g_dAcOutW = s.acOutW; g_dAcOn = s.acOutputOn;

  uint32_t now = millis();
  bool need = grid && !s.acOutputOn;

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
    Serial.println("grid present + AC output OFF -> re-arming AC output");
    oledStatus("RE-ARMING...");
    if (setAcOutput(true)) {
      delay(2000);
      State after;
      if (readState(after) && after.acOutputOn) {
        Serial.println("re-arm SUCCESS (AC output now ON)");
        g_failedRearms = 0;
        g_graceUntil = now + REARM_GRACE_MS;
        g_dAcOn = true;
        oledStatus("RE-ARMED");
      } else {
        g_failedRearms++;
        Serial.printf("re-arm did not stick (attempt %d)\n", g_failedRearms);
        oledStatus("re-arm no stick");
      }
    } else {
      g_failedRearms++;
      Serial.printf("re-arm write failed (attempt %d)\n", g_failedRearms);
      oledStatus("re-arm wr failed");
    }
  } else if (!grid) {
    oledStatus("OUTAGE (no grid)");
  } else {  // grid && s.acOutputOn
    g_failedRearms = 0;  // healthy
    oledStatus("OK - armed");
  }

  if (g_client && g_client->isConnected()) g_client->disconnect();
}

// ---- WiFi beacon probe -----------------------------------------------------
// Passwordless: scan the air and report whether the configured SSID's beacon is present.
// The router is powered by the AC200L, so "SSID on the air" => router booted => AC output on.
// No association, no password — only the (non-secret) SSID name is used. Returns false when
// the optimization is disabled (no SSID), so the caller always falls through to the BLE check.
static bool ssidBeaconPresent() {
  if (strlen(WIFI_SSID) == 0) return false;
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
    Serial.println("No WIFI_SSID set -> always check over BLE (every 5 min).");
  else
    Serial.printf("WiFi beacon optimization on for SSID '%s' (idle 15 min when seen).\n", WIFI_SSID);
  oledInit();
  g_respSem = xSemaphoreCreateBinary();
}

void loop() {
  // Security guard: with no target device configured we never touch any unit.
  if (strlen(TARGET_DEVICE_ID) == 0) {
    Serial.println("No target device ID configured; not re-arming.");
    oledStatus("No device ID set");
    delay(CHECK_INTERVAL_MS);
    return;
  }

  // WiFi beacon shortcut: SSID on the air => router up => everything fine, skip the re-arm.
  if (ssidBeaconPresent()) {
    Serial.println("SSID present -> router powered / AC output on. Idle 15 min (no re-arm).");
    oledStatus("WiFi OK - router up");
    delay(WIFI_OK_INTERVAL_MS);
    return;
  }

  // SSID not seen (or WiFi disabled): bring up BLE, check the unit, re-arm if needed.
  Serial.println("SSID not seen -> checking the AC200L over Bluetooth.");
  oledStatus("Checking BLE...");
  NimBLEDevice::init("");
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  bleCheckAndRearm();
  NimBLEDevice::deinit(true);            // free the radio so the next WiFi scan is clean
  g_client = nullptr; g_writeChar = nullptr; g_notifyChar = nullptr;

  delay(CHECK_INTERVAL_MS);
}
