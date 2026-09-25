#ifndef MODE2_SOUND_BLE_HPP
#define MODE2_SOUND_BLE_HPP

#include <ArduinoBLE.h>
#include "SoundBarDisplay.hpp"

// ============================================================
// MODE 2: SOUND INTENSITY (PHONE-ASSISTED, VIA BLE)
// ============================================================
// A phone performs sound detection/analysis externally and streams
// left/right channel intensity (0-255 each) to the Nano over BLE. This
// file is display/communication-only on the Nano side — the phone-side
// sound-analysis algorithm is developed separately (see project docs).
//
// Requires the following to already be declared in the main sketch,
// before this file is #included there:
//   currentMode          - the active DisplayMode
//   MODE_SOUND_BLE        - this mode's enum value
//   renderActiveMode()    - pushes the active mode's buffer to the matrix
// ============================================================

// ---------------- BLE service/characteristics ----------------
// UUIDs must match the phone-side app exactly.
BLEService soundService("12345678-1234-1234-1234-123456789000");
BLEByteCharacteristic leftCharacteristic("12345678-1234-1234-1234-123456789001", BLERead | BLEWrite);
BLEByteCharacteristic rightCharacteristic("12345678-1234-1234-1234-123456789002", BLERead | BLEWrite);
BLEByteCharacteristic directionCharacteristic("12345678-1234-1234-1234-123456789003", BLERead | BLEWrite);

// ---------------- State ----------------
byte soundBLELeftIntensity = 0;
byte soundBLERightIntensity = 0;
byte soundBLEDirection = 128; // informational only, not used in display logic
bool soundBLEConnected = false;

byte soundBLEMatrixState[8] = {0, 0, 0, 0, 0, 0, 0, 0};
int soundBLELastLeftLevel = -1;  // -1 forces a first draw
int soundBLELastRightLevel = -1;

// ---------------- Logic ----------------

void soundBLESetup() {
  if (!BLE.begin()) {
    Serial.println("BLE FAILED to start — sound (BLE) mode will show no data.");
    return;
  }

  BLE.setDeviceName("SoundArduinoTest");
  BLE.setLocalName("SoundArduinoTest");

  soundService.addCharacteristic(leftCharacteristic);
  soundService.addCharacteristic(rightCharacteristic);
  soundService.addCharacteristic(directionCharacteristic);
  BLE.addService(soundService);

  leftCharacteristic.writeValue(soundBLELeftIntensity);
  rightCharacteristic.writeValue(soundBLERightIntensity);
  directionCharacteristic.writeValue(soundBLEDirection);

  BLE.advertise();
  Serial.println("BLE READY — advertising as SoundArduinoTest");
}

// Non-blocking BLE handling — checked once per loop() pass so step
// tracking and mode switching keep working even while a phone is
// connected (unlike a `while (phone.connected())` style loop).
void soundBLEPoll() {
  BLE.poll();

  BLEDevice phone = BLE.central();

  if (phone && phone.connected()) {
    if (!soundBLEConnected) {
      soundBLEConnected = true;
      Serial.println("PHONE CONNECTED");
    }

    // Phone sends left, then right, then direction last per packet.
    if (leftCharacteristic.written()) {
      soundBLELeftIntensity = leftCharacteristic.value();
    }
    if (rightCharacteristic.written()) {
      soundBLERightIntensity = rightCharacteristic.value();
    }
    if (directionCharacteristic.written()) {
      soundBLEDirection = directionCharacteristic.value();

      Serial.print("Packet: L=");
      Serial.print(soundBLELeftIntensity);
      Serial.print(" R=");
      Serial.print(soundBLERightIntensity);
      Serial.print(" Dir=");
      Serial.println(soundBLEDirection);
    }
  } else if (soundBLEConnected) {
    soundBLEConnected = false;
    soundBLELeftIntensity = 0;
    soundBLERightIntensity = 0;
    soundBLEDirection = 128;
    Serial.println("PHONE DISCONNECTED");
  }
}

// Always runs every loop() iteration, regardless of active mode.
void soundBLEUpdate() {
  soundBLEPoll();

  int leftLevel = constrain(soundBLELeftIntensity / 32, 0, 7);
  int rightLevel = constrain(soundBLERightIntensity / 32, 0, 7);

  // Only rebuild/redraw if something actually changed
  if (leftLevel != soundBLELastLeftLevel || rightLevel != soundBLELastRightLevel) {
    buildIntensityBarMatrix(soundBLELeftIntensity, soundBLERightIntensity, soundBLEMatrixState);
    soundBLELastLeftLevel = leftLevel;
    soundBLELastRightLevel = rightLevel;

    if (currentMode == MODE_SOUND_BLE) {
      renderActiveMode();
    }
  }
}

#endif // MODE2_SOUND_BLE_HPP
