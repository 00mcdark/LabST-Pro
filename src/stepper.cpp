/*
 * stepper.cpp - hardware timer1 driven step generation.
 *
 * timer1 runs at 80 MHz with TIM_DIV1 -> 1 tick = 12.5 ns (80 ticks / us).
 * We use auto-reload (TIM_LOOP) and toggle STEP each interrupt, so the
 * programmed period is half the electrical step period (50% duty square wave).
 *
 * ESP8266 notes:
 *  - The ISR must carry ICACHE_RAM_ATTR or the chip crashes on first tick.
 *  - timer1_isr_init() must be called once before enabling.
 *  - timer1 max ticks = 8388607 (23-bit). 1 RPM -> 3,000,000 ticks (ok).
 */
#include "stepper.h"

// Core ESP8266 timer1 API (declared in Arduino.h / cores/esp8266/Arduino.h)
extern "C" {
  void timer1_isr_init(void);
  void timer1_attachInterrupt(void (*fn)(void));
  void timer1_detachInterrupt(void);
  void timer1_enable(uint8_t divider, uint8_t type, uint8_t reload);
  void timer1_disable(void);
  void timer1_write(uint32_t ticks);
}
#define TIM_DIV1    0     // 80 MHz -> 80 ticks / us
#define TIM_EDGE    1
#define TIM_LOOP    1

static Stepper* g_inst = nullptr;
static volatile bool g_stepState = false;

static void ICACHE_RAM_ATTR stepIsr() {
  if (!g_inst) return;
  g_stepState = !g_stepState;
  digitalWrite(PIN_STEP, g_stepState ? HIGH : LOW);
}

void Stepper::begin() {
  g_inst = this;
  pinMode(PIN_STEP,   OUTPUT);
  pinMode(PIN_DIR,    OUTPUT);
  pinMode(PIN_ENABLE, OUTPUT);
  pinMode(PIN_RESET,  OUTPUT);

  // nSLEEP is tied to 3.3V on the PCB (always awake).
  // M0/M1/M2 are tied on the PCB for fixed 1/8 microstepping.
  digitalWrite(PIN_RESET, HIGH);   // not in reset
  enableDriver(false);             // EN high = disabled (active LOW)
  digitalWrite(PIN_STEP, LOW);

  timer1_isr_init();
  timer1_disable();
  timer1_attachInterrupt(stepIsr);
  timer1_enable(TIM_DIV1, TIM_EDGE, TIM_LOOP);
  setTimerPeriodUs(1000000);       // idle ~1 s until started
}

void Stepper::enableDriver(bool on) {
  digitalWrite(PIN_ENABLE, on ? LOW : HIGH);
}

void Stepper::hardResetDriver() {
  digitalWrite(PIN_RESET, LOW);
  delay(10);
  digitalWrite(PIN_RESET, HIGH);
  delay(2);
}

void Stepper::setDirection(bool cw) {
  dirCw = cw;
  digitalWrite(PIN_DIR, cw ? HIGH : LOW);
}

void Stepper::setTargetRpm(int rpm) {
  tgtRpm = constrain(rpm, RPM_MIN, RPM_MAX);
}

void Stepper::setTimerPeriodUs(uint32_t us) {
  // half period (toggle each interrupt), clamp to timer1 23-bit range
  uint32_t halfUs = us / 2;
  if (halfUs < 2) halfUs = 2;
  uint32_t ticks = halfUs * 80UL;          // 80 ticks per us @ 80 MHz
  if (ticks > 8388607UL) ticks = 8388607UL;
  timer1_write(ticks);
}

void Stepper::applyInterval() {
  long stepsPerSec = RPM_TO_STEPS_PER_SEC(curRpm);
  if (stepsPerSec <= 0) {
    setTimerPeriodUs(1000000);  // ~1 s idle
    return;
  }
  uint32_t periodUs = 1000000UL / (uint32_t)stepsPerSec;
  setTimerPeriodUs(periodUs);
}

void Stepper::start() {
  if (tgtRpm <= 0) return;
  hardResetDriver();
  digitalWrite(PIN_DIR, dirCw ? HIGH : LOW);  // latch direction
  enableDriver(true);
  running = true;
  stopping = false;
  if (softRamp) {
    rampFromRpm = 0;
    rampStartMs = millis();
    curRpm = 0;
  } else {
    curRpm = tgtRpm;
  }
  applyInterval();
}

void Stepper::stop(bool soft) {
  if (!running) return;
  if (soft && softRamp) {
    stopping = true;
    rampFromRpm = curRpm;
    rampStartMs = millis();
  } else {
    running = false;
    stopping = false;
    enableDriver(false);   // coast, less heat
  }
}

void Stepper::emergencyStop() {
  running = false;
  stopping = false;
  enableDriver(false);
  curRpm = 0;
  tgtRpm = 0;
  g_stepState = false;
  digitalWrite(PIN_STEP, LOW);
}

void Stepper::update() {
  if (!running) return;

  if (softRamp && !stopping) {
    uint32_t elapsed = millis() - rampStartMs;
    if (elapsed < SOFT_ACCEL_TIME_MS) {
      curRpm = rampFromRpm + ((tgtRpm - rampFromRpm) * (long)elapsed) / SOFT_ACCEL_TIME_MS;
    } else {
      curRpm = tgtRpm;
    }
    applyInterval();
  } else if (softRamp && stopping) {
    uint32_t elapsed = millis() - rampStartMs;
    if (elapsed < SOFT_ACCEL_TIME_MS) {
      curRpm = rampFromRpm - (rampFromRpm * (long)elapsed) / SOFT_ACCEL_TIME_MS;
      if (curRpm < 0) curRpm = 0;
    } else {
      curRpm = 0;
    }
    applyInterval();
    if (curRpm <= 0) {
      running = false;
      stopping = false;
      enableDriver(false);
    }
  }
}
