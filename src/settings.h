/*
 * settings.h - persistent, wear-leveled settings stored as JSON in LittleFS.
 */
#ifndef SETTINGS_H
#define SETTINGS_H

#include <Arduino.h>

struct Settings {
  uint32_t magic = 0x4C415253;   // "LARS"
  int      rpm          = 300;
  bool     direction    = true;  // true = CW
  bool     softStart    = true;
  bool     softStop     = true;
  bool     potControl   = false; // if true, pot overrides rpm
  uint32_t timerSec     = 0;     // 0 = no timer
  bool     enabled      = false; // driver enabled by default after boot? no -> false
  int      restartCount = 0;
};

bool loadSettings(Settings &s);
bool saveSettings(const Settings &s);

#endif
