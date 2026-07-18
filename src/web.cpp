/*
 * web.cpp - synchronous web server (ESP8266WebServer), REST API and OTA.
 * Serves static files from LittleFS and exposes a JSON control API.
 *
 * Uses the core ESP8266WebServer (no external async dependency) so the
 * build stays simple and reliable. Call server.handleClient() in loop().
 */
#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "config.h"
#include "settings.h"
#include "stepper.h"

// Helpers defined in main.cpp (declared before use in this TU)
int  readPotRpm();
void doStallRecovery();

extern Stepper stirrer;
extern Settings settings;
extern bool timerActive;
extern uint32_t timerEndMs;
extern float lastTempC;
extern int   stallRestarts;

ESP8266WebServer server(80);

// --- small helpers -------------------------------------------------------
static String readFile(const String &path) {
  File f = LittleFS.open(path, "r");
  if (!f) return "";
  String s = f.readString();
  f.close();
  return s;
}

static void sendJson(const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void setupWeb() {
  // Serve static files; default index.html
  server.onNotFound([]() {
    String p = server.uri();
    if (p.endsWith("/")) p += "index.html";
    if (LittleFS.exists(p)) {
      server.send(200, "text/html", readFile(p));
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });

  // ---- API: status ----
  server.on("/api/status", HTTP_GET, []() {
    JsonDocument doc;
    doc["running"]     = stirrer.isRunning();
    doc["rpm"]         = stirrer.currentRpm();
    doc["target"]      = stirrer.targetRpm();
    doc["dir"]         = settings.direction ? "CW" : "CCW";
    doc["softStart"]   = settings.softStart;
    doc["softStop"]    = settings.softStop;
    doc["pot"]         = settings.potControl;
    doc["timerSec"]    = settings.timerSec;
    doc["timerActive"] = timerActive;
    doc["tempC"]       = lastTempC;
    doc["restarts"]    = stallRestarts;
    doc["uptime"]      = millis() / 1000;
    sendJson(doc);
  });

  // ---- API: set parameters ----
  server.on("/api/set", HTTP_POST, []() {
    if (server.hasArg("plain")) {
      JsonDocument doc;
      DeserializationError err = deserializeJson(doc, server.arg("plain"));
      if (!err) {
        JsonObject obj = doc.as<JsonObject>();
        if (obj["rpm"].is<int>())       settings.rpm = constrain(obj["rpm"].as<int>(), RPM_MIN, RPM_MAX);
        if (obj["dir"].is<const char*>()) settings.direction = obj["dir"].as<String>() == "CW";
        if (obj["softStart"].is<bool>()) settings.softStart = obj["softStart"].as<bool>();
        if (obj["softStop"].is<bool>())  settings.softStop  = obj["softStop"].as<bool>();
        if (obj["pot"].is<bool>())       settings.potControl = obj["pot"].as<bool>();
        if (obj["timerSec"].is<uint32_t>()) settings.timerSec = obj["timerSec"].as<uint32_t>();
        saveSettings(settings);
      }
    }
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: start ----
  server.on("/api/start", HTTP_POST, []() {
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
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: stop (soft) ----
  server.on("/api/stop", HTTP_POST, []() {
    stirrer.stop(settings.softStop);
    timerActive = false;
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: emergency stop ----
  server.on("/api/estop", HTTP_POST, []() {
    stirrer.emergencyStop();
    timerActive = false;
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: toggle direction ----
  server.on("/api/dir", HTTP_POST, []() {
    settings.direction = !settings.direction;
    stirrer.setDirection(settings.direction);
    saveSettings(settings);
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- API: stall recovery (power-cycle 12V rail, resume previous) ----
  server.on("/api/recover", HTTP_POST, []() {
    doStallRecovery();
    server.send(200, "application/json", "{\"ok\":true}");
  });

  // ---- OTA firmware upload (GET shows form, POST receives binary) ----
  server.on("/update", HTTP_GET, []() {
    String html = F("<html><body style='font-family:system-ui;background:#0f1419;color:#e6edf3;padding:40px'>"
      "<h2>LabStir Firmware Update (OTA)</h2>"
      "<form method='post' action='/update' enctype='multipart/form-data'>"
      "<input type='file' name='firmware' accept='.bin'><br><br>"
      "<button type='submit' style='padding:10px 20px'>Upload &amp; Flash</button>"
      "</form><p><a href='/' style='color:#27c08a'>&laquo; Back</a></p></body></html>");
    server.send(200, "text/html", html);
  });
  server.on("/update", HTTP_POST, []() {
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    ESP.restart();
  }, []() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
      Update.runAsync(true);
      Update.begin((ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (upload.currentSize) Update.write(upload.buf, upload.currentSize);
    } else if (upload.status == UPLOAD_FILE_END) {
      Update.end(true);
    }
  });

  server.begin();
}
