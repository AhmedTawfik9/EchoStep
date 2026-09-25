#ifndef BAND_PASS_FILTER_HPP
#define BAND_PASS_FILTER_HPP

#include<Arduino.h>
#include <math.h>
#include <cstdint>
#include"Structures.h"

// Configuring BandPassFilter for sound direction detection

// ============================================================
// Measurement configuration
// ============================================================

// 100 ms RMS measurement
// const uint32_t RMS_SAMPLES = SAMPLE_RATE / 10;

// Five approximately octave-wide bands:
//
//        Band          Center
//      250-500 Hz       354 Hz
//      500-1 kHz        707 Hz
//      1-2 kHz         1414 Hz
//      2-4 kHz         2828 Hz
//      4-8 kHz         5657 Hz

const int NUM_BANDS = 5;

const float CENTER_FREQUENCIES[NUM_BANDS] = {
  353.553f,
  707.107f,
  1414.214f,
  2828.427f,
  5656.854f
};

// Q corresponding approximately to one octave bandwidth
const float FILTER_Q = 1.41421356f;


// ============================================================
// Band-pass filter
// ============================================================

class BPFilter {

public:

  int sampleRate;

  float b0, b1, b2;
  float a1, a2;

  float x1;
  float x2;
  float y1;
  float y2;

  BPFilter(){
    x1 = x2 = 0;
    y1 = y2 = 0;
  }

  void begin(int sampleRate, float centerFrequency, float Q) {

    this->sampleRate = sampleRate;
    
    float w0 = 2.0f * PI * centerFrequency / sampleRate;

    float cosW0 = cosf(w0);
    float sinW0 = sinf(w0);

    float alpha = sinW0 / (2.0f * Q);

    // RBJ constant-skirt band-pass filter
    float B0 = sinW0 / 2.0f;
    float B1 = 0.0f;
    float B2 = -sinW0 / 2.0f;

    float A0 = 1.0f + alpha;
    float A1 = -2.0f * cosW0;
    float A2 = 1.0f - alpha;

    // Normalize coefficients
    b0 = B0 / A0;
    b1 = B1 / A0;
    b2 = B2 / A0;

    a1 = A1 / A0;
    a2 = A2 / A0;

    reset();
  }

  void reset() {

    x1 = 0;
    x2 = 0;
    y1 = 0;
    y2 = 0;
  }

  float process(float x) {

    float y =
      b0 * x +
      b1 * x1 +
      b2 * x2 -
      a1 * y1 -
      a2 * y2;

    x2 = x1;
    x1 = x;

    y2 = y1;
    y1 = y;

    return y;
  }
};

// ============================================================
// Band-pass Filters Setup
// ============================================================

// Band-pass filters with above described center frequencies
BPFilter filters[NUM_BANDS];

void BPFReset(int sampleRate) {
  // Initialize filters
  for (int b = 0; b < NUM_BANDS; b++) {

    filters[b].begin(
      sampleRate,
      CENTER_FREQUENCIES[b],
      FILTER_Q
    );
  }
}

void BPFInit(int sampleRate) {

  Serial.println();
  Serial.println("======================================");
  Serial.println(" SAMPLER Multi-Band RMS Sound Analyzer");
  Serial.println("======================================");

  Serial.println();

  Serial.print("Sample rate: ");
  Serial.print(sampleRate);
  Serial.println(" Hz");

  int RMS_SAMPLES = sampleRate / 10;
  Serial.print("RMS window: ");
  Serial.print((1000.0f * RMS_SAMPLES) / sampleRate);
  Serial.println(" ms");

  Serial.println();

  Serial.println("Bands:");

  for (int b = 0; b < NUM_BANDS; b++) {

    Serial.print("  ");
    Serial.print(b);
    Serial.print(": ");
    Serial.print(CENTER_FREQUENCIES[b], 1);
    Serial.println(" Hz");
  }

  Serial.println();
  
  BPFReset(sampleRate);
}


// ============================================================
// RMS accumulation
// ============================================================

// Accumulated squared values
double sumSquares[NUM_BANDS] = {0, 0, 0, 0, 0};

// Number of samples accumulated
volatile uint32_t sampleCount = 0;

// Indicates that a complete RMS measurement is ready
volatile bool measurementReady = false;

// Results from the last completed measurement
volatile float rmsValues[NUM_BANDS];

// ============================================================
// Filter Data and perform RMS Sum
// ============================================================

void filterRMSData(const float buffer [] , int buffer_size, FrequencyMagnitude mic []) {

  for (int i = 0; i < buffer_size; i++) {

    float x = (float)buffer[i];

    // Process this sample through every band-pass filter
    for (int b = 0; b < NUM_BANDS; b++) {

      float filtered = filters[b].process(x);
      sumSquares[b] +=
        (double)filtered * (double)filtered;
    }

    sampleCount++;

    // 100 ms measurement complete
    int rms_samples = filters[0].sampleRate / 10;
    if ((int)(sampleCount) >= rms_samples) {

      for (int b = 0; b < NUM_BANDS; b++) {

        rmsValues[b] =
          sqrt(sumSquares[b] / (double)sampleCount);
      }

      // Reset accumulators
      for (int b = 0; b < NUM_BANDS; b++) {
        sumSquares[b] = 0;
      }

      sampleCount = 0;
    }
  }

  // populate frequency-magnitude pairs in array
  for (int b = 0; b < NUM_BANDS; b++) {
    mic[b].frequency = CENTER_FREQUENCIES[b];
    mic[b].magnitude = rmsValues[b];
  }
}


#endif // BAND_PASS_FILTER_HPP
