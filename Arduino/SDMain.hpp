#ifndef SD_MAIN_HPP
#define SD_MAIN_HPP

#include <Arduino.h>
#include "Correction.hpp"
#include "BandPassFilter.hpp"
#include "Structures.h"
#include "SoundDirection.hpp"
#include "Sampler.h"

const int MIC1_PIN = A6;
const int MIC2_PIN = A7;

const int SAMPLE_RATE = 16000;
const int SAMPLER_BUFFER_SIZE = 1600; // 100 ms of samples per channel
int16_t adc_buffer[2 * SAMPLER_BUFFER_SIZE]; // One buffer for two channels

// Processing buffers
float buffer_mic1[SAMPLER_BUFFER_SIZE];
float buffer_mic2[SAMPLER_BUFFER_SIZE];

bool measurements_ready = false;
bool sdapp_print = false;
bool micIntensityUpdated = false; // set true once per completed 100ms cycle

// forward-declare callback functions
void mics_read();

Sampler mics(SAMPLE_RATE, SAMPLER_BUFFER_SIZE, adc_buffer, mics_read);

// Array of BP-filtered RMS magnitudes for each microphone
FrequencyMagnitude fm_mic1 [NUM_BANDS];
FrequencyMagnitude fm_mic2 [NUM_BANDS];

// Weighs for sum of RMS-BP-filtered frquency-magnitude data
const float FREQUENCY_WEIGHTS[NUM_BANDS] = {0.1, 0.15, 0.15, 0.25, 0.35};
float power_mic1 = 0.0;
float power_mic2 = 0.0;

// TODO: CALIBRATE these before final deployment. These are placeholder
// values only — wired in now so the classifier's noise-floor filtering
// is actually active during testing, rather than silently disabled (it
// defaults to 0.0f per band, which effectively never filters anything).
// To calibrate: enable sdapp_print below, sit in a quiet room, note the
// typical per-band magnitude values reported for both mics during
// silence, and set these thresholds a bit above that observed floor.
const float NOISE_THRESHOLD_PLACEHOLDER[NUM_BANDS] = {5.0f, 5.0f, 5.0f, 5.0f, 5.0f};

// Separate ceilings per mic, since the two channels showed different
// maximum observed power during live testing (mic1/RIGHT topped out ~0.5,
// mic2/LEFT ~0.7) — likely differing mic sensitivity, mounting, or distance
// from the test source. Using one shared ceiling made equally loud sounds
// read differently depending on which side they came from; separate
// ceilings make both sides read consistently for equally loud sound.
// Each includes a bit of headroom above its own observed maximum.
const float MAX_POWER_RIGHT = 0.6f; // mic1, observed max ~0.5
const float MAX_POWER_LEFT = 0.8f;  // mic2, observed max ~0.7

SoundDirectionClassifier directionClassifier;

// Holds the latest classification result. Declared here (not computed via
// a call to update() at global scope) because global initializers run
// before setup() — calling update() here would run the classifier against
// uninitialized hardware/buffers before anything is configured. As a
// global with static storage duration, this is zero-initialized at
// program start (Direction::LEFT == 0, all floats == 0.0f), which is a
// safe default until the first real update() call happens inside SDLoop().
SoundDirectionClassifier::Result sdapp_result;

void mics_read(){
  measurements_ready = true;
}

void convert_mics(){
  for (uint32_t i = 0; i < SAMPLER_BUFFER_SIZE; ++i) {
    buffer_mic1[i] =
        adc_buffer[2 * i] * (3.3f / 4095.0f);

    buffer_mic2[i] =
        adc_buffer[2 * i + 1] * (3.3f / 4095.0f);
  }
}

void SDSetup() {
  analogReadResolution(12);

  // NOTE: Serial.begin() and any wait-for-Serial-Monitor logic are owned by
  // the main sketch's setup() (dual_mode_ble.ino) once this is combined into
  // the full project — not repeated here, to avoid stacking multiple
  // blocking while(!Serial) waits across the merged modes.

  delay(2000); // brief warm-up delay before starting mic sampling

  // Setup Sound-Direction Classifier
  directionClassifier.setMicrophoneSpacing(0.057f);
  directionClassifier.setSoundSpeed(343.0f);
  directionClassifier.setCorrelationEnabled(true);
  directionClassifier.setSpectralWeight(0.65f);
  directionClassifier.setCorrelationWeight(0.35f);
  directionClassifier.setEnterThreshold(0.35f);
  directionClassifier.setLeaveThreshold(0.15f);
  directionClassifier.setDirectionPersistence(2);
  directionClassifier.setCenterPersistence(3);
  directionClassifier.setMinimumCorrelationStrength(0.20f);
  directionClassifier.setNoiseThresholds(NOISE_THRESHOLD_PLACEHOLDER, NUM_BANDS); // TODO: replace with calibrated values

  BPFInit(SAMPLE_RATE);
  mics.start();

  Serial.println("Sampler Started!");
}

void SDLoop() {
  // Check whether the hardware ADC/DMA has completed the 100 ms acquisition.
  mics.poll();

  if (measurements_ready) {
    measurements_ready = false;

    // Convert raw ADC int data to float
    convert_mics();

    // Serial.println("First Buffer!");

    // filter and get RMS band values
    filterRMSData(buffer_mic1, SAMPLER_BUFFER_SIZE, fm_mic1);
    filterRMSData(buffer_mic2, SAMPLER_BUFFER_SIZE, fm_mic2);
    
    // Serial.println("Filtered!");

    //calibrate frequency-magnitude pairs for comparability
    correctFreqMag(fm_mic1, NUM_BANDS, MIC1);
    correctFreqMag(fm_mic2, NUM_BANDS, MIC2);

    sdapp_result = directionClassifier.update(
      fm_mic1, fm_mic2,
      NUM_BANDS, FREQUENCY_WEIGHTS,
      adc_buffer,
      SAMPLER_BUFFER_SIZE, SAMPLE_RATE);

    // Overall per-mic loudness: weighted sum of band magnitudes (same
    // FREQUENCY_WEIGHTS used for direction scoring, reused here so "loud"
    // means the same thing for both the direction estimate and the
    // intensity readout).
    power_mic1 = 0.0f;
    power_mic2 = 0.0f;
    for (int b = 0; b < NUM_BANDS; b++) {
      power_mic1 += FREQUENCY_WEIGHTS[b] * fm_mic1[b].magnitude;
      power_mic2 += FREQUENCY_WEIGHTS[b] * fm_mic2[b].magnitude;
    }

    micIntensityUpdated = true;
    
    if(sdapp_print){
      // Print result
      Serial.print("result: ");
      Serial.print(sdapp_result.direction);
      Serial.print(" Score: ");
      Serial.println(sdapp_result.score);

      // Print band data
      Serial.print("Mic1: ");
      for (auto b = 0; b < NUM_BANDS; ++b){
        Serial.print(" ");
        Serial.print(b);
        Serial.print(": ");
        Serial.print(fm_mic1[b].magnitude);
      }
      Serial.println();
      Serial.print("Mic2: ");
      for (auto b = 0; b < NUM_BANDS; ++b){
        Serial.print(" ");
        Serial.print(b);
        Serial.print(": ");
        Serial.print(fm_mic2[b].magnitude);
      }
      Serial.println();
    }

    // Restart filters
    BPFReset(SAMPLE_RATE);

    // Restart sampler
    mics.start();
  }
}

// ------------------------------------------------------------
// Final output: left/right intensity, 0-255, matching the same range
// used by the BLE-based sound mode (leftCharacteristic/rightCharacteristic
// in dual_mode_ble.ino), so mode #3 can plug into the existing display
// logic without any changes there.
//
// MIC1 (A6) = RIGHT, MIC2 (A7) = LEFT — per the physical convention
// documented in SoundDirection.hpp. If your wiring is reversed, swap
// which mic feeds which function below rather than rewiring the board.
// ------------------------------------------------------------

byte getLeftIntensity() {
  float normalized = power_mic2 / MAX_POWER_LEFT; // MIC2 = LEFT
  normalized = constrain(normalized, 0.0f, 1.0f);
  return (byte)(normalized * 255.0f);
}

byte getRightIntensity() {
  float normalized = power_mic1 / MAX_POWER_RIGHT; // MIC1 = RIGHT
  normalized = constrain(normalized, 0.0f, 1.0f);
  return (byte)(normalized * 255.0f);
}

#endif // SD_MAIN_HPP
