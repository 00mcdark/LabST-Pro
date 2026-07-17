# Pro Lab Overhead Stirrer — NodeMCU + DRV8825 + NEMA17

A commercial-grade lab overhead stirrer controller built around a NodeMCU
(ESP8266) access-point web interface, a DRV8825 stepper driver and a
MOONS 17HD8001-02 (NEMA17 1.8°, 1.2 A/phase, 3.5 kg·cm) motor.

This document covers **wiring, the PCB / bill of materials, noise reduction,
and how to build & flash the firmware**. The firmware source is in `src/`,
the web UI in `data/`.

---

## 1. Why 1/8 microstepping (not full-step)?

| Mode        | Steps/rev | Smoothness | Torque ripple | Noise | Max usable RPM |
|-------------|-----------|------------|--------------|-------|----------------|
| Full (1)    | 200       | Poor       | High         | Loud  | Higher per-pulse efficiency |
| **1/8**     | **1600**  | **Smooth** | **Low**      | **Quiet** | Excellent for a stirrer |

For a **lab stirrer** the dominant requirements are *smooth low-speed torque*
and *low vibration/noise*, not raw top speed. 1/8 microstepping gives the
smoothest rotation and best average torque across 1–1500 RPM while the
ESP8266 hardware timer easily generates the required pulse rates
(1500 RPM × 1600 steps/rev ÷ 60 = 40 000 steps/s, a 25 µs period — handled
by `timer1` in `src/stepper.cpp`).

The microstep pins are **tied on the PCB** (M0=1, M1=1, M2=0) so firmware
never changes them.

---

## 2. Pin map (NodeMCU / ESP-12E)

| Function            | NodeMCU | GPIO | DRV8825 / device | Notes |
|---------------------|---------|------|------------------|-------|
| STEP                | D5      | 14   | STEP             | PWM from timer1 |
| DIR                 | D6      | 12   | DIR              | |
| ENABLE (active LOW) | D8      | 15   | ENABLE           | HIGH = disabled (coast) |
| nRESET             | D7      | 13   | RESET            | HIGH = run |
| nSLEEP            | –       | –    | SLEEP            | **Tie to 3.3 V** via 10 kΩ pull-up (always awake) |
| M0                  | –       | –    | M0               | **Tie to 3.3 V** (1) |
| M1                  | –       | –    | M1               | **Tie to 3.3 V** (1) |
| M2                  | –       | –    | M2               | **Tie to GND** (0) |
| Potentiometer (wiper)| A0   | A0   | –                | 10 kΩ pot, ends to 3.3 V / GND |
| START button        | D1      | 5    | –                | Tactile, other leg to GND (internal pull-up) |
| STOP button         | D2      | 4    | –                | Tactile, other leg to GND |
| DIRECTION button    | D0      | 16   | –                | Tactile, other leg to GND |
| Status / heartbeat LED | D4  | 2    | –                | On-board LED (or external + 330 Ω) |
| Power relay         | D3      | 0    | Relay coil driver| **Keep 10 kΩ pull-up to 3.3 V** (boot-strap) |
| NTC (optional)      | A0*     | A0   | –                | Via CD4051 mux or dedicated divider (see §7) |

\* The ESP8266 has a single ADC. The **potentiometer owns A0 by default**.
NTC thermal sensing is disabled in firmware (`NTC_ENABLED 0`) until you add a
CD4051 analog mux (or a second divider) — see §7.

---

## 3. Wiring diagram (logical)

```
12 V / 10 A supply
  +12V ──┬───────────────► DRV8825 VMOT
         │
        [EMI filter]  (ferrite bead / common-mode choke in the +12V line)
         │
      [Reverse-polarity protection]  (P-channel MOSFET or Schottky diode)
         │
      [TVS diode]  (SMAJ15A, cathode to +12V, anode to GND)  ← surge clamp
         │
   ┌─────┴─────┐
   │  RELAY    │  (coil driven by NodeMCU GPIO0 through a transistor/MOSFET)
   │  (NO)     │
   └─────┬─────┘
         ├──► DRV8825 VMOT
         └──► (motor supply only; logic stays on)

DRV8825
  GND   ── GND (shared with NodeMCU GND)
  VDD   ── 3.3 V (from NodeMCU regulator or external 3.3 V LDO)
  STEP  ── D5 (GPIO14)
  DIR   ── D6 (GPIO12)
  EN    ── D8 (GPIO15)
  RESET ── D7 (GPIO13)
  SLEEP ── 3.3 V (10 kΩ pull-up)
  M0    ── 3.3 V
  M1    ── 3.3 V
  M2    ── GND
  1A/1B ── Motor coil A (A+/A-)
  2A/2B ── Motor coil B (B+/B-)
  SLP  (internal)

Current set: adjust DRV8825 Vref trimpot to **0.50 V** (≈1.0 A/phase).
  Formula: I_trip = Vref / (5 × Rsense), Rsense = 0.1 Ω  →  Vref = 0.5 V.
  (Module max for the MOONS 1.2 A part; 1.0 A keeps the driver cool.)

MOONS 17HD8001-02 (4-wire bipolar)
  A+ / A- → 1A / 1B
  B+ / B- → 2A / 2B
  (If the motor jitters, swap one coil pair.)

NodeMCU
  3.3 V ── DRV8825 VDD, pull-ups, pot top
  GND   ── common ground (star-point at supply GND)
  A0    ── pot wiper (10 kΩ pot between 3.3 V and GND)
  D1/D2/D0 ── buttons to GND (INPUT_PULLUP)
  D4    ── status LED
  D3    ── relay driver (with 10 kΩ pull-up to 3.3 V)
```

---

## 4. Power & protection parts (noise / reliability)

| Purpose | Part | Value / type | Where |
|---------|------|--------------|-------|
| Reverse polarity | P-ch MOSFET (e.g. AO3401) or Schottky (1N5822) | – | Between supply +12 V and load |
| TVS surge clamp | SMAJ15A (or 15 V Zener) | 15 V | Across +12 V / GND at input |
| EMI filter | Ferrite bead / common-mode choke + 100 nF X-cap | – | In +12 V line near inlet |
| Bulk cap (input) | Electrolytic | 470–1000 µF, 25 V | Across +12 V / GND at DRV |
| Local cap (motor driver) | Ceramic | 100 nF + 10 µF | Right at DRV8825 VMOT/GND |
| Logic decoupling | Ceramic | 100 nF | At every VDD pin (NodeMCU, DRV VDD) |
| DRV8825 sense | On-module | 0.1 Ω (default) | Sets current; do not remove |
| Relay flyback | Diode | 1N4007 | Across relay coil (cathode to +V) |
| Motor term. RC snubber | Resistor + cap | 100 Ω + 100 nF | Across each motor coil (kills HF ringing) |
| Status LED | LED + R | 330 Ω | D4 to GND |
| Potentiometer | 10 kΩ linear | – | Wiper → A0 |
| Buttons | Tactile | – | To GND, INPUT_PULLUP |

### Recommended extra filtering for clean motor operation
- **100 nF ceramic across VMOT/GND** *and* a **10 µF tantalum** at the DRV8825.
- **100 Ω + 100 nF RC snubber across each motor coil** — dramatically reduces
  the high-frequency ringing that couples into the 12 V rail and the ADC.
- **Star grounding**: motor/12 V return meets logic GND at a single point to
  avoid injecting motor current into the ESP's ground (a classic source of
  resets and Wi-Fi dropouts).
- **Separate the 3.3 V logic** with a small LDO (e.g. AMS1117-3.3) fed from
  the 12 V rail, with its own 10 µF + 100 nF decoupling.
- **Ferrite bead on the STEP/DIR lines** (e.g. 600 Ω@100 MHz) if you see
  radiated EMI; keep those traces short and away from the motor wires.

---

## 5. Front-panel layout
- **Potentiometer** – speed (1–1500 RPM) when "Potentiometer control" is on.
- **START** button – begin run (applies soft-start ramp).
- **STOP** button – soft stop, clears timer.
- **DIRECTION** button – toggles CW / CCW (also in web UI).
- **Status LED** – heartbeat (blinks while firmware is healthy); steady/fast
  blink indicates a fault.

---

## 6. PCB / enclosure recommendations
- 2-layer board, ≥1 oz copper. Keep **motor/12 V traces wide** (≥2 mm for
  10 A; better: pour a 12 V plane).
- Put the DRV8825 on a **header** with a heatsink; it dissipates the most heat.
- Keep the NodeMCU away from the motor wires; mount it in a shielded corner.
- Drill vent holes; the driver and motor get warm at high current.
- Silkscreen the pin names from §2 directly on the board.
- Add a **reset button** and a **user LED** on the front panel.
- Connect the motor shaft to the stirrer shaft via a flexible coupling; mount
  the whole assembly on a rigid base.

---

## 7. NTC temperature protection (optional)
The ESP8266 has one ADC (A0), used by the speed pot. To add thermal
protection you have two options:

1. **CD4051 analog mux** on A0, selected by two spare GPIOs, switching
   between the pot and the NTC divider. Enable it in firmware by setting
   `NTC_ENABLED 1` in `platformio.ini` `build_flags` and reading the mux
   output in `readNTC()` (`src/main.cpp`).
2. **Dedicated divider** on A0 and move the pot to a digital encoder
   (recommended for a "pro" unit).

NTC circuit: 3.3 V → 10 kΩ pull-up → A0 → NTC (10 kΩ @25 °C, B=3950) → GND.
Over-temp threshold `TEMP_LIMIT_C = 70` in `config.h` triggers an automatic
emergency stop.

---

## 8. Firmware features (mapped to your requirements)

| Requirement | Implemented in |
|-------------|----------------|
| Always-on AP `labst` / `mc142536` | `main.cpp` `WiFi.softAP(...)` |
| Web UI + REST API | `web.cpp`, `data/index.html` |
| CW / CCW 1–1500 RPM | `stepper.cpp`, web slider |
| Soft start / stop (ramps) | `stepper.cpp::update()` |
| Emergency stop (immediate disable) | `/api/estop`, `emergencyStop()` |
| Enable/disable driver + timeout | `enableDriver()`, coast after stop |
| Timer 1 min – 24 h | `timerSec` (slider, seconds) |
| Stall → power-cycle & resume | `doStallRecovery()` (`/api/recover`) |
| Potentiometer speed | `readPotRpm()` + `potControl` flag |
| OTA firmware update | `/update` (web form + async flash) |
| LittleFS settings (wear friendly) | `settings.cpp` JSON |
| Watchdog (firmware lockup recovery) | `onWatchdog()` Ticker |
| Brown-out predictability | internal BOD + 200 ms startup delay |
| Heartbeat LED | `onHeartbeat()` |
| Acceleration/deceleration | `SOFT_ACCEL_TIME_MS` ramp |
| Current matched to motor | DRV8825 Vref = 0.50 V (1.0 A) |

> **Stall note:** True stall detection needs an encoder or current sensor.
> The firmware provides the *recovery action* (`/api/recover` and the web
> button) and a max-retry lockout (`STALL_RESTARTS_MAX`). Add a hall/encoder
> on the shaft and call `doStallRecovery()` automatically when no pulses are
> detected over a window.

---

## 9. Build & flash

### Prerequisites
- [PlatformIO](https://platformio.org/) (`pip install platformio` or VSCode
  extension).
- A USB-to-serial adapter (or the NodeMCU's built-in one).

### Steps
```bash
# 1. Build the firmware
pio run

# 2. Upload the firmware (connect NodeMCU via USB)
pio run -t upload

# 3. Upload the web UI to LittleFS (must be done once / after UI changes)
pio run -t uploadfs
```
After booting, the controller creates the Wi-Fi network **labst** (password
**mc142536**). Connect with a phone/PC, open `http://192.168.4.1/`.

### OTA update later (no USB, no opening the box)
1. In the web UI click **Firmware Update (OTA)** → upload a fresh
   `firmware.bin` (from `.pio/build/nodemcu/firmware.bin`).
2. Or from the CLI: `pio run -t upload --upload-port http://192.168.4.1/update`
   (password `mc142536`).

---

## 10. First-run checklist
1. Set DRV8825 Vref to **0.50 V** with no motor load.
2. Power from the 12 V/10 A supply; confirm 3.3 V logic is stable.
3. Connect to `labst` Wi-Fi; open the UI.
4. Press **START** at low RPM (e.g. 100) and verify smooth rotation & direction.
5. Increase to 1500 RPM; if the motor stalls/audible, the microstep setting or
   Vref may need adjustment.
6. Test **STOP**, **E-Stop**, **DIRECTION**, the **pot**, and set a short
   **timer** to confirm auto-stop.
7. Confirm the status LED blinks (heartbeat) continuously.

---

## 11. File layout
```
platformio.ini        build config
src/config.h          pins, motor math, behaviour constants
src/settings.h/.cpp   persistent JSON settings (LittleFS)
src/stepper.h/.cpp    timer1 step generation + ramps
src/web.cpp           async web server, REST API, OTA endpoint
src/main.cpp          AP, loop, buttons, pot, NTC, timer, watchdog, recovery
data/index.html       single-page web UI
README.md             this file
```
