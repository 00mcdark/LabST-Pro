/*
 * stepper.h - DRV8825 step pulse generator using ESP8266 hardware timer1.
 *
 * Step pulses are produced by timer1 in auto-reload mode at microsecond
 * resolution. At 1500 RPM the step period is only ~25 us, which a 1 ms
 * software Ticker cannot achieve, hence the hardware timer. The interrupt
 * toggles the STEP pin, so the timer period is set to half the step period
 * (50% duty square wave).
 *
 * Acceleration / deceleration ramps ease speed changes (soft start/stop).
 */
#ifndef STEPPER_H
#define STEPPER_H

#include <Arduino.h>

class Stepper {
public:
  void begin();
  void setDirection(bool cw);          // true = CW
  bool getDirection() const { return dirCw; }
  void setTargetRpm(int rpm);          // desired steady-state RPM
  void setSoftRamp(bool on) { softRamp = on; }
  void start();
  void stop(bool soft = true);         // soft = ramp down
  void emergencyStop();                // immediate disable
  void enableDriver(bool on);          // DRV ENABLE pin (active LOW)
  void hardResetDriver();              // RESET+SLEEP toggle to clear faults
  bool isRunning() const { return running; }
  int  currentRpm() const { return curRpm; }
  int  targetRpm() const { return tgtRpm; }
  void update();                       // call from loop() for ramp progress

private:
  void applyInterval();                // recompute timer period from curRpm
  void setTimerPeriodUs(uint32_t us);  // program timer1

  int  tgtRpm = 0;
  int  curRpm = 0;
  bool running = false;
  bool softRamp = true;
  bool stopping = false;
  uint32_t rampStartMs = 0;
  int      rampFromRpm = 0;
  bool     dirCw = true;
};

#endif
