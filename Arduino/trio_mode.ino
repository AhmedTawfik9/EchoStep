/*
  Triple-Mode Wearable Feedback Device
  ----------------------------------------
  Three display modes on a single 8x8 MAX7219 matrix, cycled via the B14
  button (interrupt-driven):

    MODE_STEPS      - step-count progress (onboard IMU)          -> Mode1_Steps.hpp
    MODE_SOUND_BLE  - sound intensity via phone-assisted BLE      -> Mode2_SoundBLE.hpp
    MODE_MIC_ARRAY  - sound direction/intensity, onboard 2-mic    -> Mode3_MicArray.hpp

  Each mode lives entirely in its own file: its own tuning constants,
  state, and update logic, grouped together and documented at the top of
  that file. This file only owns shared hardware (the matrix, the
  button) and orchestration (mode enum, ISR, setup/loop, render dispatch)
  — see each Mode*.hpp file for that mode's specific tuning parameters
  and calibration notes.

  CORE DESIGN PRINCIPLE: every mode's data is updated EVERY loop
  iteration regardless of which mode is currently displayed. Only the
  final "push this mode's buffer to the matrix" step is gated on which
  mode is active (see renderActiveMode() and each mode's own Update()
  function). This keeps all three subsystems "live" in the background,
  so switching modes never loses data.

  The button uses a hardware interrupt only because it's an
  unpredictable, user-triggered event we don't want to risk missing
  (e.g. during the step mode's celebration delay() calls). The ISR
  itself only flips a flag; the actual mode switch and re-render happen
  in loop() — see onButtonPress() and the top of loop().

  Wiring:
  - MAX7219 matrix (SPI): VCC->5V, GND->GND, DIN->D11, CLK->D13, CS->D10
  - B14 button: one leg -> D2 (BUTTON_PIN below), other leg -> GND
    (uses internal pullup, so button press = LOW)
  - Two B03 microphones: MIC1 -> A6 (must be the RIGHT mic), MIC2 -> A7
    (must be the LEFT mic) — see Mode3_MicArray.hpp / SoundDirection.hpp
    for why this physical placement matters.

  NOTE ON MAX7219 POWER: if the matrix shows nothing, the Nano's 3.3V
  logic may be below the MAX7219's input-high threshold when it's
  powered at 5V. Try powering the matrix's VCC from 3.3V instead, or use
  a logic level shifter.
*/

#include <MD_MAX72xx.h>
#include <SPI.h>

// ---------------- MAX7219 matrix configuration ---------------- PAROLA_HW or GENERIC_HW or FC16_HW
#define HARDWARE_TYPE MD_MAX72XX::GENERIC_HW
#define MAX_DEVICES 1
#define CS_PIN 10
MD_MAX72XX mx = MD_MAX72XX(HARDWARE_TYPE, CS_PIN, MAX_DEVICES);

// ---------------- Display mode ----------------
enum DisplayMode { MODE_STEPS, MODE_SOUND_BLE, MODE_MIC_ARRAY, NUM_MODES };
DisplayMode currentMode = MODE_STEPS;

// ---------------- Button configuration ----------------
const int BUTTON_PIN = 2;
const unsigned long DEBOUNCE_MS = 200;
volatile bool modeChangeRequested = false;
volatile unsigned long lastInterruptTime = 0;

void onButtonPress() {
  unsigned long now = millis();
  if (now - lastInterruptTime > DEBOUNCE_MS) {
    modeChangeRequested = true;
    lastInterruptTime = now;
  }
}

// Forward declaration so the mode files (included below) can call this
// from their own Update() functions.
void renderActiveMode();

// ---------------- Mode implementations ----------------
// Each file is self-contained: its own tuning constants, state, and
// logic. See each file's header comment for details specific to that
// mode.
#include "Mode1_Steps.hpp"
#include "Mode2_SoundBLE.hpp"
#include "Mode3_MicArray.hpp"

void setup() {
  Serial.begin(115200);

  // Bounded wait for Serial Monitor rather than an infinite while(!Serial)
  // — lets the sketch proceed even if no Serial connection shows up,
  // instead of hanging forever. This is the ONE Serial wait for the whole
  // sketch; individual mode files do not repeat it.
  unsigned long serialWaitStart = millis();
  while (!Serial && (millis() - serialWaitStart < 3000)) {
    ;
  }

  bool matrixOK = mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, MAX_INTENSITY);
  mx.clear();
  Serial.print("MAX7219 init: ");
  Serial.println(matrixOK ? "OK" : "FAILED");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), onButtonPress, FALLING);

  stepsSetup();
  soundBLESetup();
  micArraySetup();

  Serial.println("Ready. Starting in STEP mode.");
  renderActiveMode();
}

void loop() {
  // Handle a pending mode switch requested by the ISR
  if (modeChangeRequested) {
    modeChangeRequested = false;
    currentMode = (DisplayMode)((currentMode + 1) % NUM_MODES);

    Serial.print("Switched to mode: ");
    switch (currentMode) {
      case MODE_STEPS:     Serial.println("STEPS"); break;
      case MODE_SOUND_BLE: Serial.println("SOUND (BLE)"); break;
      case MODE_MIC_ARRAY: Serial.println("SOUND (MIC ARRAY)"); break;
      default: break;
    }
    renderActiveMode();
  }

  // ALWAYS update every mode's data, regardless of active mode
  stepsUpdate();
  soundBLEUpdate();
  micArrayUpdate();
}

// Pushes the currently active mode's buffer to the physical matrix.
// This is the ONLY place that writes to the matrix during normal
// operation (stepsCelebrateMilestone() is the sole deliberate exception,
// since it's a direct full-matrix flash effect gated on steps being
// active at the time it's called).
void renderActiveMode() {
  byte *buffer;

  switch (currentMode) {
    case MODE_STEPS:     buffer = stepMatrixState; break;
    case MODE_SOUND_BLE: buffer = soundBLEMatrixState; break;
    case MODE_MIC_ARRAY: buffer = micArrayMatrixState; break;
    default: return;
  }

  for (int row = 0; row < 8; row++) {
    mx.setRow(0, row, buffer[row]);
  }
}
