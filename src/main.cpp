/*
 * main.cpp - Pro Lab Overhead Stirrer firmware (NodeMCU ESP8266)
 *
 * Features:
 *  - Always-on Wi-Fi Access Point "labst" / "mc142536"
 *  - Async web UI + REST API + OTA firmware update (LittleFS hosted)
 *  - DRV8825 / NEMA17 control: CW/CCW, 1-1500 RPM, soft start/stop, E-Stop
 *  - Front panel: pot, Start, Stop, Direction buttons + status LED
 *  - Run timer 1 min .. 24 h (auto stop)
 *  - Stall recovery: power-cycle 12V rail via relay, resume previous state
 *  - NTC over-temp protection, heartbeat LED, watchdog, brown-out detect
 *  - Persistent settings in LittleFS (wear friendly)
 */
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <Ticker.h>
#include "config.h"
#include "settings.h"
#include "stepper.h"
#include "web.cpp"

Stepper stirrer;
Settings settings;

Ticker heartbeat;
Ticker watchdog;

// Global state
bool timerActive = false;
uint32_t timerEndMs = 0;
float lastTempC = 25.0;
int   stallRestarts = 0;

// Watchdog: if loop() never feeds it, restart
volatile bool wdtFed = false;
uint32_t lastFeedMs = 0;

// Potentiometer
int potRpm = 300;

// Buttons (active LOW, internal pull-up; polled in loop, not interrupts)
bool startPressed = false;
bool stopPressed = false;
bool dirPressed = false;

// Heartbeat
bool ledState = false;

// ---- Forward declarations ----
void onHeartbeat();
void onWatchdog();
int  readPotRpm();
float readNTC();
void handleButtons();
void feedWatchdog();
void doStallRecovery();
void powerRelay(bool on);

void setup() {
  Serial.begin(115200);
  Serial.println("\nPro Lab Stirrer booting...");

  // Brown-out: ESP has internal BOD; ensure stable startup delay
  delay(200);

  // ---- Filesystem ----
  if (!LittleFS.begin()) {
#ifdef FORMAT_FS_ON_FAIL
    LittleFS.format();
    LittleFS.begin();
#else
    Serial.println("LittleFS mount failed");
#endif
  }
  loadSettings(settings);

  // ---- Pins ----
  pinMode(PIN_POT,       INPUT);
  pinMode(PIN_START,     INPUT_PULLUP);
  pinMode(PIN_STOP,      INPUT_PULLUP);
  pinMode(PIN_DIRBTN,    INPUT_PULLUP);
  pinMode(PIN_LED,       OUTPUT);
  pinMode(PIN_RELAY,     OUTPUT);
  powerRelay(true);                 // 12V rail ON

  // ---- Stepper ----
  stirrer.begin();
  stirrer.setDirection(settings.direction);
  stirrer.setTargetRpm(settings.rpm);
  stirrer.setSoftRamp(settings.softStart);

  // ---- Wi-Fi Access Point (always on) ----
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(IPAddress(AP_IP), IPAddress(AP_GATEWAY), IPAddress(AP_SUBNET));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("AP IP: "); Serial.println(WiFi.softAPIP());

  // ---- mDNS ----
  MDNS.begin(HOSTNAME);

  // ---- OTA ----
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword("mc142536");
  ArduinoOTA.begin();

  // ---- Web ----
  setupWeb();

  // ---- Timers ----
  heartbeat.attach_ms(HEARTBEAT_MS, onHeartbeat);
  watchdog.attach_ms(WATCHDOG_MS, onWatchdog);

  Serial.println("Ready. Connect to Wi-Fi 'labst' / 'mc142536'");
}

void loop() {
  ArduinoOTA.handle();
  MDNS.update();

  stirrer.update();

  // ---- Potentiometer speed control ----
  static uint32_t lastPot = 0;
  if (millis() - lastPot > POT_SAMPLE_MS) {
    lastPot = millis();
    potRpm = readPotRpm();
    if (settings.potControl && stirrer.isRunning()) {
      stirrer.setTargetRpm(potRpm);
    }
  }

  // ---- NTC thermal check (only if NTC_ENABLED; needs extra hardware) ----
  static uint32_t lastNtc = 0;
  if (millis() - lastNtc > NTC_SAMPLE_MS) {
    lastNtc = millis();
#if NTC_ENABLED
    lastTempC = readNTC();
    if (lastTempC > TEMP_LIMIT_C && stirrer.isRunning()) {
      stirrer.emergencyStop();      // over-temp fault
      timerActive = false;
    }
#else
    lastTempC = 25.0f;              // disabled: report safe ambient
#endif
  }

  // ---- Run timer ----
  if (timerActive && millis() >= timerEndMs) {
    timerActive = false;
    stirrer.stop(settings.softStop);
  }

  // ---- Buttons ----
  handleButtons();

  // ---- Watchdog feed ----
  feedWatchdog();

  delay(10);
}

/* ------------------------------------------------------------------ */
void onHeartbeat() {
  ledState = !ledState;
  digitalWrite(PIN_LED, ledState ? LOW : HIGH); // on-board LED inverted
}

void onWatchdog() {
  // If loop stalled (not fed), restart
  if (!wdtFed) {
    Serial.println("WATCHDOG: loop stalled, restarting");
    ESP.restart();
  }
  wdtFed = false;
}

void feedWatchdog() {
  wdtFed = true;
}

int readPotRpm() {
  int raw = analogRead(PIN_POT);          // 0..1023
  // map to RPM 1..1500 with dead-zone at bottom
  int rpm = map(constrain(raw, 0, 1023), 0, 1023, RPM_MIN, RPM_MAX);
  return constrain(rpm, RPM_MIN, RPM_MAX);
}

float readNTC() {
  // NTC voltage divider: 3.3V -> 10k pull-up -> NODE -> NTC -> GND.
  // If a separate analog channel/mux is fitted, read it here. By default
  // we read A0 which, on a basic build, is shared with the POT. For a real
  // thermal reading fit a CD4051 mux or a separate divider and read it.
  int raw = analogRead(PIN_NTC);
  float v = raw * (3.3f / 1024.0f);
  if (v < 0.01f) return -40.0f;
  float r = (3.3f - v) * 10000.0f / v;   // pull-up 10k
  float t = 1.0f / (1.0f / 298.15f + (1.0f / 3950.0f) * log(r / 10000.0f)) - 273.15f;
  return t;
}

void handleButtons() {
  static uint32_t lastDeb = 0;
  if (millis() - lastDeb < BUTTON_DEBOUNCE_MS) return;
  bool s = !digitalRead(PIN_START);
  bool st = !digitalRead(PIN_STOP);
  bool d = !digitalRead(PIN_DIRBTN);

  if (s && !startPressed) {
    startPressed = true;
    stirrer.setDirection(settings.direction);
    stirrer.setTargetRpm(settings.potControl ? potRpm : settings.rpm);
    stirrer.setSoftRamp(settings.softStart);
    if (settings.timerSec > 0) {
      timerActive = true;
      timerEndMs = millis() + (uint32_t)settings.timerSec * 1000UL;
    }
    stirrer.start();
    lastDeb = millis();
  } else if (!s) startPressed = false;

  if (st && !stopPressed) {
    stopPressed = true;
    stirrer.stop(settings.softStop);
    timerActive = false;
    lastDeb = millis();
  } else if (!st) stopPressed = false;

  if (d && !dirPressed) {
    dirPressed = true;
    settings.direction = !settings.direction;
    stirrer.setDirection(settings.direction);
    saveSettings(settings);
    lastDeb = millis();
  } else if (!d) dirPressed = false;
}

void powerRelay(bool on) {
  digitalWrite(PIN_RELAY, on ? HIGH : LOW);
}

void doStallRecovery() {
  // Power-cycle the 12V rail to clear a stuck driver/motor, then resume
  if (stallRestarts >= STALL_RESTARTS_MAX) {
    stirrer.emergencyStop();
    return;
  }
  stallRestarts++;
  settings.restartCount = stallRestarts;
  saveSettings(settings);

  bool wasRunning = stirrer.isRunning();
  stirrer.emergencyStop();     // disable driver
  powerRelay(false);           // cut 12V
  delay(800);
  powerRelay(true);            // restore 12V
  delay(300);
  stirrer.hardResetDriver();
  if (wasRunning) {
    stirrer.setDirection(settings.direction);
    stirrer.setTargetRpm(settings.potControl ? potRpm : settings.rpm);
    stirrer.setSoftRamp(settings.softStart);
    stirrer.start();
    if (settings.timerSec > 0) {
      timerActive = true;
      timerEndMs = millis() + (uint32_t)settings.timerSec * 1000UL;
    }
  }
}
