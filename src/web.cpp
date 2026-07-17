/*
 * web.cpp - Async web server, REST API and OTA update endpoint.
 * Serves static files from LittleFS and exposes a JSON control API.
 */
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "settings.h"
#include "stepper.h"

extern Stepper stirrer;
extern Settings settings;
extern bool timerActive;
extern uint32_t timerEndMs;
extern float lastTempC;
extern int   stallRestarts;

AsyncWebServer server(80);

void setupWeb() {
  server.serveStatic("/", LittleFS, "/").setDefaultFile("index.html");

  // ---- API: status ----
  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *r){
    DynamicJsonDocument doc(512);
    doc["running"]   = stirrer.isRunning();
    doc["rpm"]       = stirrer.currentRpm();
    doc["target"]    = stirrer.targetRpm();
    doc["dir"]       = settings.direction ? "CW" : "CCW";
    doc["softStart"] = settings.softStart;
    doc["softStop"]  = settings.softStop;
    doc["pot"]       = settings.potControl;
    doc["timerSec"]  = settings.timerSec;
    doc["timerActive"] = timerActive;
    doc["tempC"]     = lastTempC;
    doc["restarts"]  = stallRestarts;
    doc["uptime"]    = millis() / 1000;
    String out;
    serializeJson(doc, out);
    r->send(200, "application/json", out);
  });

  // ---- API: set parameters ----
  AsyncCallbackJsonWebHandler* setHandler = new AsyncCallbackJsonWebHandler("/api/set", [](AsyncWebServerRequest *r, JsonVariant &json){
    JsonObject obj = json.as<JsonObject>();
    if (obj.containsKey("rpm"))        settings.rpm = constrain(obj["rpm"].as<int>(), RPM_MIN, RPM_MAX);
    if (obj.containsKey("dir"))        settings.direction = obj["dir"].as<String>() == "CW";
    if (obj.containsKey("softStart"))  settings.softStart = obj["softStart"].as<bool>();
    if (obj.containsKey("softStop"))   settings.softStop  = obj["softStop"].as<bool>();
    if (obj.containsKey("pot"))        settings.potControl = obj["pot"].as<bool>();
    if (obj.containsKey("timerSec"))   settings.timerSec = obj["timerSec"].as<uint32_t>();
    saveSettings(settings);
    String out = "{\"ok\":true}";
    r->send(200, "application/json", out);
  });
  server.addHandler(setHandler);

  // ---- API: start ----
  server.on("/api/start", HTTP_POST, [](AsyncWebServerRequest *r){
    stirrer.setDirection(settings.direction);
    stirrer.setTargetRpm(settings.potControl ? readPotRpm() : settings.rpm);
    stirrer.setSoftRamp(settings.softStart);
    if (settings.timerSec > 0) {
      timerActive = true;
      timerEndMs = millis() + (uint32_t)settings.timerSec * 1000UL;
    } else {
      timerActive = false;
    }
    stirrer.start();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: stop (soft) ----
  server.on("/api/stop", HTTP_POST, [](AsyncWebServerRequest *r){
    stirrer.stop(settings.softStop);
    timerActive = false;
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: emergency stop ----
  server.on("/api/estop", HTTP_POST, [](AsyncWebServerRequest *r){
    stirrer.emergencyStop();
    timerActive = false;
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: toggle direction ----
  server.on("/api/dir", HTTP_POST, [](AsyncWebServerRequest *r){
    settings.direction = !settings.direction;
    stirrer.setDirection(settings.direction);
    saveSettings(settings);
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: stall recovery (power-cycle 12V rail, resume previous) ----
  server.on("/api/recover", HTTP_POST, [](AsyncWebServerRequest *r){
    doStallRecovery();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  // ---- OTA firmware upload (GET shows form, POST receives binary) ----
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *r){
    String html = F("<html><body style='font-family:system-ui;background:#0f1419;color:#e6edf3;padding:40px'>"
      "<h2>LabStir Firmware Update (OTA)</h2>"
      "<form method='post' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin'><br><br>"
      "<button type='submit' style='padding:10px 20px'>Upload &amp; Flash</button>"
      "</form><p><a href='/' style='color:#27c08a'>&laquo; Back</a></p></body></html>");
    r->send(200, "text/html", html);
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *r){
    r->send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final){
    if (!index) {
      Update.runAsync(true);
      Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    }
    if (len) Update.write(data, len);
    if (final) Update.end(true);
  });

  server.begin();
}

// Forward-declared helpers defined in main.cpp
int  readPotRpm();
void doStallRecovery();
