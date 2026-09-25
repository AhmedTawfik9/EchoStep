#ifndef SOUND_BAR_DISPLAY_HPP
#define SOUND_BAR_DISPLAY_HPP

#include <Arduino.h>

// ============================================================
// Shared sound-intensity bar-graph renderer.
// ============================================================
// Used by any mode that produces a left/right intensity pair in the
// common 0-255 format (currently: Mode 2 - BLE, and Mode 3 - mic array).
// Sharing this avoids duplicating the same display logic per mode.
//
// Layout (8x8 matrix):
//   columns 0-2 = LEFT bar
//   columns 3-4 = always off (visual gap between channels)
//   columns 5-7 = RIGHT bar
// Each side has 8 levels (one per row), filled bottom-up, representing
// intensity ranges of 32 each: level 1 = 0-31, level 2 = 32-63, ...,
// level 8 = 224-255.
// ============================================================
void buildIntensityBarMatrix(byte leftIntensity, byte rightIntensity, byte outBuffer[8]) {
  int leftLevel = constrain(leftIntensity / 32, 0, 7);
  int rightLevel = constrain(rightIntensity / 32, 0, 7);

  int leftNumLit = leftLevel + 1;   // 1-8 LEDs lit (even level 0 shows one LED)
  int rightNumLit = rightLevel + 1;

  for (int row = 0; row < 8; row++) {
    byte rowByte = 0;

    bool leftOn = (7 - row) < leftNumLit;
    bool rightOn = (7 - row) < rightNumLit;

    if (leftOn) {
      rowByte |= (1 << 0) | (1 << 1) | (1 << 2); // columns 0,1,2
    }
    if (rightOn) {
      rowByte |= (1 << 5) | (1 << 6) | (1 << 7); // columns 5,6,7
    }
    // columns 3,4 intentionally left as 0 (always off, the middle gap)

    outBuffer[row] = rowByte;
  }
}

#endif // SOUND_BAR_DISPLAY_HPP
