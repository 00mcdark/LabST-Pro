/*
 * settings.cpp - JSON backed settings in LittleFS.
 * Written infrequently (only on user changes) to limit flash wear.
 */
#include "settings.h"
#include <LittleFS.h>
#include <ArduinoJson.h>

bool loadSettings(Settings &s) {
  if (!LittleFS.exists(SETTINGS_FILE)) return false;
  File f = LittleFS.open(SETTINGS_FILE, "r");
  if (!f) return false;
  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;
  if (doc["magic"] != s.magic) return false; // different layout -> ignore
  s.rpm         = doc["rpm"]      | s.rpm;
  s.direction   = doc["dir"]      | s.direction;
  s.softStart   = doc["sstart"]   | s.softStart;
  s.softStop    = doc["sstop"]    | s.softStop;
  s.potControl  = doc["pot"]      | s.potControl;
  s.timerSec    = doc["timer"]    | s.timerSec;
  s.enabled     = doc["enabled"]  | s.enabled;
  s.restartCount= doc["rc"]       | s.restartCount;
  return true;
}

bool saveSettings(const Settings &s) {
  DynamicJsonDocument doc(1024);
  doc["magic"]   = s.magic;
  doc["rpm"]     = s.rpm;
  doc["dir"]     = s.direction;
  doc["sstart"]  = s.softStart;
  doc["sstop"]   = s.softStop;
  doc["pot"]     = s.potControl;
  doc["timer"]   = s.timerSec;
  doc["enabled"] = s.enabled;
  doc["rc"]      = s.restartCount;
  File f = LittleFS.open(SETTINGS_FILE, "w");
  if (!f) return false;
  serializeJson(doc, f);
  f.close();
  return true;
}
