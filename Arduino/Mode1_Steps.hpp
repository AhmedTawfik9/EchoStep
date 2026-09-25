#ifndef MODE1_STEPS_HPP
#define MODE1_STEPS_HPP

#include <Arduino_BMI270_BMM150.h>

// ============================================================
// MODE 1: STEP COUNTER
// ============================================================
// Uses the onboard IMU (BMI270) to detect steps via accelerometer peak
// detection, and shows progress as a two-tier milestone system across
// all 64 matrix LEDs: every STEPS_PER_LED steps lights one more LED
// (cumulative, reading order); once all 64 are lit, a celebration
// animation plays and the display resets.
//
// Requires the following to already be declared in the main sketch,
// before this file is #included there:
//   mx                 - the MD_MAX72XX matrix object
//   currentMode         - the active DisplayMode
//   MODE_STEPS          - this mode's enum value
//   renderActiveMode()  - pushes the active mode's buffer to the matrix
// ============================================================

// ---------------- Tuning parameters ----------------
// TUNE ON THE FINAL WEARABLE: recalibrate STEP_THRESHOLD once mounted in
// the enclosure, since mounting position/orientation changes the walking
// acceleration profile the IMU sees.
const float STEP_THRESHOLD = 1.4f;              // g's above baseline to count as a step peak
const unsigned long MIN_STEP_INTERVAL_MS = 300; // debounce: min time between counted steps
const float SMOOTHING_ALPHA = 0.3f;             // low-pass filter strength (0-1, higher = less smoothing)
const int STEPS_PER_LED = 1;                   // small milestone: steps needed to light one more LED
const int TOTAL_LEDS = 64;                      // fixed: 8x8 matrix
const long BIG_MILESTONE_STEPS = (long)STEPS_PER_LED * TOTAL_LEDS; // fixed, derived

// ---------------- State ----------------
float stepsSmoothedMagnitude = 1.0f;
bool stepsAbovePeak = false;
unsigned long stepsLastStepTime = 0;
unsigned long stepCount = 0;
byte stepMatrixState[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int stepsLitLedCount = 0;

// ---------------- Logic ----------------

void stepsLightNextLED() {
  if (stepsLitLedCount >= TOTAL_LEDS) return;
  int row = stepsLitLedCount / 8;
  int col = stepsLitLedCount % 8;
  stepMatrixState[row] |= (1 << col);
  stepsLitLedCount++;
}

void stepsResetMatrix() {
  for (int i = 0; i < 8; i++) stepMatrixState[i] = 0;
  stepsLitLedCount = 0;
}

// Flashes the whole matrix 3 times when the big milestone is reached.
// Writes directly to the matrix (bypassing the buffer) since this is only
// ever called when steps mode is confirmed active — see stepsUpdate().
void stepsCelebrateMilestone() {
  for (int flash = 0; flash < 3; flash++) {
    for (int row = 0; row < 8; row++) {
      mx.setRow(0, row, 0xFF);
    }
    delay(150);
    mx.clear();
    delay(150);
  }
}

void stepsSetup() {
  if (!IMU.begin()) {
    Serial.println("Failed to initialize IMU!");
    while (1); // halt — check board selection in IDE
  }

  Serial.print("Accelerometer sample rate: ");
  Serial.print(IMU.accelerationSampleRate());
  Serial.println(" Hz");
}

// Always runs every loop() iteration, regardless of active mode, so step
// count keeps advancing in the background while another mode is displayed.
void stepsUpdate() {
  float x, y, z;

  if (IMU.accelerationAvailable()) {
    IMU.readAcceleration(x, y, z); // values in g's

    float magnitude = sqrt(x * x + y * y + z * z);
    stepsSmoothedMagnitude = SMOOTHING_ALPHA * magnitude + (1 - SMOOTHING_ALPHA) * stepsSmoothedMagnitude;

    unsigned long now = millis();

    if (stepsSmoothedMagnitude > STEP_THRESHOLD && !stepsAbovePeak) {
      stepsAbovePeak = true;

      if (now - stepsLastStepTime > MIN_STEP_INTERVAL_MS) {
        stepCount++;
        stepsLastStepTime = now;

        Serial.print("Step count: ");
        Serial.println(stepCount);

        bool ledAdded = false;
        if (stepCount % STEPS_PER_LED == 0) {
          stepsLightNextLED();
          ledAdded = true;
        }

        if (stepsLitLedCount >= TOTAL_LEDS) {
          if (currentMode == MODE_STEPS) stepsCelebrateMilestone();
          stepsResetMatrix();
          Serial.print(">> BIG milestone reached at ");
          Serial.print(stepCount);
          Serial.println(" steps!");
          ledAdded = true;
        }

        // Only touch the physical display if steps mode is active
        if (ledAdded && currentMode == MODE_STEPS) {
          renderActiveMode();
        }
      }
    } else if (stepsSmoothedMagnitude < STEP_THRESHOLD) {
      stepsAbovePeak = false;
    }
  }
}

#endif // MODE1_STEPS_HPP
