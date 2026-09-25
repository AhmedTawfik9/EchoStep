#ifndef MODE3_MIC_ARRAY_HPP
#define MODE3_MIC_ARRAY_HPP

#include "SDMain.hpp"
#include "SoundBarDisplay.hpp"

// ============================================================
// MODE 3: SOUND DIRECTION (TWO-MICROPHONE ARRAY, ONBOARD)
// ============================================================
// Local sound direction/intensity estimation using two B03 microphones,
// sampled directly via the nRF52840's SAADC hardware (Sampler.h/.cpp),
// band-pass filtered (BandPassFilter.hpp) and classified
// (SoundDirection.hpp / SoundDirectionClassifier). Produces the same
// 0-255 left/right intensity format as Mode 2, via getLeftIntensity()/
// getRightIntensity() (defined in SDMain.hpp), so it reuses the same
// bar-graph rendering (buildIntensityBarMatrix) as Mode 2.
//
// TUNING / CALIBRATION — to be finalized once mounted in the 3D printed
// enclosure, since mounting position affects mic exposure/sensitivity:
//   - MAX_POWER_LEFT / MAX_POWER_RIGHT   (in SDMain.hpp) — per-mic
//     ceilings scaling raw power to 0-255. Re-measure after mounting.
//   - NOISE_THRESHOLD_PLACEHOLDER        (in SDMain.hpp) — per-band
//     noise floor; currently placeholders, re-measure once mounted.
//   - _centerBias[] (SoundDirectionClassifier constructor, in
//     SoundDirection.hpp) — per-band calibration correcting inherent
//     mic mismatch; re-measure if mounting/enclosure changes acoustic
//     balance between the two mics.
//   - Wiring convention: MIC1 (A6) = RIGHT, MIC2 (A7) = LEFT — confirmed
//     already correct for the current wiring (see SoundDirection.hpp).
//
// Requires the following to already be declared in the main sketch,
// before this file is #included there:
//   currentMode          - the active DisplayMode
//   MODE_MIC_ARRAY        - this mode's enum value
//   renderActiveMode()    - pushes the active mode's buffer to the matrix
// ============================================================

byte micArrayMatrixState[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int micArrayLastLeftLevel = -1;
int micArrayLastRightLevel = -1;

void micArraySetup() {
  sdapp_print = false; // set true for per-band Serial debug output during calibration
  SDSetup();
}

// Always runs every loop() iteration, regardless of active mode.
void micArrayUpdate() {
  SDLoop(); // polls the hardware sampler; only does real work once per ~100ms window

  if (micIntensityUpdated) {
    micIntensityUpdated = false;

    byte leftIntensity = getLeftIntensity();
    byte rightIntensity = getRightIntensity();

    int leftLevel = constrain(leftIntensity / 32, 0, 7);
    int rightLevel = constrain(rightIntensity / 32, 0, 7);

    // Only rebuild/redraw if something actually changed
    if (leftLevel != micArrayLastLeftLevel || rightLevel != micArrayLastRightLevel) {
      buildIntensityBarMatrix(leftIntensity, rightIntensity, micArrayMatrixState);
      micArrayLastLeftLevel = leftLevel;
      micArrayLastRightLevel = rightLevel;

      if (currentMode == MODE_MIC_ARRAY) {
        renderActiveMode();
      }
    }
  }
}

#endif // MODE3_MIC_ARRAY_HPP
