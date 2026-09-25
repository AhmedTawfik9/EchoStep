#ifndef SOUND_DIRECTION_HPP
#define SOUND_DIRECTION_HPP

#include <Arduino.h>
#include <math.h>
#include <stdint.h>

#include "Structures.h"

class SoundDirectionClassifier {
public:
    enum Direction { LEFT, CENTER, RIGHT };

    struct Result {
        Direction direction;
        float score;             // -1 = LEFT, +1 = RIGHT
        float confidence;        // 0..1, evidence strength
        float agreement;         // 0..1, spectral band agreement
        float correlation;       // -1..+1 normalized correlation peak
        float correlationScore;  // -1..+1 directional correlation score
        float delaySamples;      // positive = RIGHT, negative = LEFT
        uint8_t activeBands;
    };

    SoundDirectionClassifier()
        : _state(CENTER),
          _leftCandidateCount(0),
          _rightCandidateCount(0),
          _centerCandidateCount(0),
          _enterThreshold(0.30f),
          _leaveThreshold(0.15f),
          _directionPersistence(2),
          _centerPersistence(3),
          _correlationEnabled(true),
          _correlationWeight(0.35f),
          _spectralWeight(0.65f),
          _microphoneSpacingM(0.050f),
          _soundSpeedMps(343.0f),
          _maxCorrelationLag(4),
          _minCorrelationStrength(0.20f),
          _epsilon(1.0e-9f)
    {
        reset();

        for (uint8_t i = 0; i < MAX_BANDS; ++i)
            _centerBias[i] = 0.0f;

        _centerBias[0] = -0.28f;
        _centerBias[1] = -0.08f;
        _centerBias[2] = -0.37f;
        _centerBias[3] = -0.51f;
        _centerBias[4] = -0.09f;
    }

    // --------------------------------------------------------
    // Main update function.
    //
    // mic1/mic2:
    //     Already calibrated FrequencyMagnitude arrays.
    //
    // numBands:
    //     Number of valid frequency bands.
    //
    // bandWeights:
    //     Weights supplied by main.cpp.
    //
    // adcBuffer:
    //     Interleaved raw samples:
    //
    //       MIC1[0], MIC2[0],
    //       MIC1[1], MIC2[1],
    //       ...
    //
    // samplePairs:
    //     Number of samples PER microphone.
    //
    // sampleRate:
    //     ADC sample rate.
    // --------------------------------------------------------
    Result update(
        const FrequencyMagnitude* mic1,
        const FrequencyMagnitude* mic2,
        uint8_t numBands,
        const float* bandWeights,
        const int16_t* adcBuffer,
        uint32_t samplePairs,
        uint32_t sampleRate)
    {
        if (numBands == 0 || bandWeights == nullptr ||
            mic1 == nullptr || mic2 == nullptr) {
            return makeResult(0.0f, 0.0f, 0.0f, 0, 0.0f, 0.0f);
        }

        if (numBands > MAX_BANDS)
            numBands = MAX_BANDS;

        float weightedScore = 0.0f;
        float totalWeight = 0.0f;
        float balances[MAX_BANDS];

        for (uint8_t b = 0; b < numBands; ++b) {
            balances[b] = 0.0f;

            float m1 = mic1[b].magnitude;
            float m2 = mic2[b].magnitude;

            if (!isfinite(m1) || !isfinite(m2))
                continue;

            if (m1 < 0.0f) m1 = 0.0f;
            if (m2 < 0.0f) m2 = 0.0f;

            float energy = m1 + m2;
            float weight = bandWeights[b];

            if (weight <= 0.0f)
                continue;

            // Ignore bands below their configured noise floor.
            if (energy < _noiseThreshold[b])
                continue;

            float balance = (m1 - m2) / (energy + _epsilon);
            // Remove the empirically measured CENTER bias.
            balance -= _centerBias[b];
            balance = clamp(balance, -1.0f, 1.0f);

            balances[b] = balance;
            weightedScore += weight * balance;
            totalWeight += weight;
        }

        float spectralScore = 0.0f;
        uint8_t activeBands = 0;

        if (totalWeight > _epsilon) {
            spectralScore = weightedScore / totalWeight;

            for (uint8_t b = 0; b < numBands; ++b) {
                if (bandWeights[b] <= 0.0f)
                    continue;

                float m1 = mic1[b].magnitude;
                float m2 = mic2[b].magnitude;

                if (!isfinite(m1) || !isfinite(m2))
                    continue;

                if (m1 < 0.0f) m1 = 0.0f;
                if (m2 < 0.0f) m2 = 0.0f;

                if ((m1 + m2) >= _noiseThreshold[b])
                    activeBands++;
            }
        }

        spectralScore = clamp(spectralScore, -1.0f, 1.0f);

        // ----------------------------------------------------
        // Spectral band agreement.
        // ----------------------------------------------------

        float agreement = 0.0f;

        if (totalWeight > _epsilon && fabsf(spectralScore) > _epsilon) {
            float direction = spectralScore > 0.0f ? 1.0f : -1.0f;
            float agreementSum = 0.0f;

            for (uint8_t b = 0; b < numBands; ++b) {
                if (bandWeights[b] <= 0.0f)
                    continue;

                float m1 = mic1[b].magnitude;
                float m2 = mic2[b].magnitude;

                if (!isfinite(m1) || !isfinite(m2))
                    continue;

                if (m1 < 0.0f) m1 = 0.0f;
                if (m2 < 0.0f) m2 = 0.0f;

                if ((m1 + m2) < _noiseThreshold[b])
                    continue;

                float directionalAgreement =
                    balances[b] * direction;

                directionalAgreement =
                    clamp(directionalAgreement, 0.0f, 1.0f);

                agreementSum +=
                    bandWeights[b] * directionalAgreement;
            }

            agreement = agreementSum / totalWeight;
        }

        // ----------------------------------------------------
        // Cross-correlation.
        // ----------------------------------------------------

        float correlation = 0.0f;
        float correlationScore = 0.0f;
        float delaySamples = 0.0f;

        if (_correlationEnabled &&
            adcBuffer != nullptr &&
            samplePairs >= 8 &&
            sampleRate > 0) {

            calculateCorrelation(
                adcBuffer,
                samplePairs,
                sampleRate,
                correlation,
                correlationScore,
                delaySamples
            );
        }

        // ----------------------------------------------------
        // Combine spectral and correlation direction.
        //
        // Both features use the same convention:
        //
        //     -1 = LEFT
        //      0 = CENTER
        //     +1 = RIGHT
        // ----------------------------------------------------

        float finalScore = spectralScore;

        bool correlationUsable =
            _correlationEnabled &&
            fabsf(correlation) >= _minCorrelationStrength;

        if (correlationUsable) {
            float totalFeatureWeight =
                _spectralWeight + _correlationWeight;

            if (totalFeatureWeight > _epsilon) {
                finalScore =
                    (spectralScore * _spectralWeight +
                     correlationScore * _correlationWeight) /
                    totalFeatureWeight;
            }
        }

        finalScore = clamp(finalScore, -1.0f, 1.0f);

        // ----------------------------------------------------
        // Confidence.
        //
        // Spectral confidence:
        //     directional strength × band agreement
        //
        // Correlation confidence:
        //     absolute correlation strength
        //
        // These are combined according to the feature weights.
        // ----------------------------------------------------

        float spectralConfidence =
            fabsf(spectralScore) * agreement;

        float correlationConfidence =
            correlationUsable ? fabsf(correlation) : 0.0f;

        float confidence;

        if (correlationUsable) {
            float totalFeatureWeight =
                _spectralWeight + _correlationWeight;

            confidence =
                (spectralConfidence * _spectralWeight +
                 correlationConfidence * _correlationWeight) /
                (totalFeatureWeight + _epsilon);
        } else {
            confidence = spectralConfidence;
        }

        // Don't allow a single active band to report maximum
        // confidence.
        if (numBands > 0) {
            float bandFactor =
                sqrtf((float)activeBands / (float)numBands);

            confidence *= bandFactor;
        }

        confidence = clamp(confidence, 0.0f, 1.0f);

        // ----------------------------------------------------
        // State transition.
        // ----------------------------------------------------

        updateState(finalScore);

        Result result;
        result.direction = _state;
        result.score = finalScore;
        result.confidence = confidence;
        result.agreement = agreement;
        result.correlation = correlation;
        result.correlationScore = correlationScore;
        result.delaySamples = delaySamples;
        result.activeBands = activeBands;

        return result;
    }

    // --------------------------------------------------------
    // Configuration
    // --------------------------------------------------------

    void setNoiseThreshold(uint8_t band, float threshold)
    {
        if (band >= MAX_BANDS)
            return;

        _noiseThreshold[band] =
            threshold < 0.0f ? 0.0f : threshold;
    }

    void setNoiseThresholds(const float* thresholds, uint8_t count)
    {
        if (thresholds == nullptr)
            return;

        if (count > MAX_BANDS)
            count = MAX_BANDS;

        for (uint8_t i = 0; i < count; ++i)
            setNoiseThreshold(i, thresholds[i]);
    }

    void setEnterThreshold(float threshold)
    {
        _enterThreshold =
            clamp(threshold, 0.0f, 1.0f);
    }

    void setLeaveThreshold(float threshold)
    {
        _leaveThreshold =
            clamp(threshold, 0.0f, 1.0f);
    }

    void setDirectionPersistence(uint8_t count)
    {
        _directionPersistence =
            count == 0 ? 1 : count;
    }

    void setCenterPersistence(uint8_t count)
    {
        _centerPersistence =
            count == 0 ? 1 : count;
    }

    // Enable/disable the correlation feature.
    void setCorrelationEnabled(bool enabled)
    {
        _correlationEnabled = enabled;
    }

    bool correlationEnabled() const
    {
        return _correlationEnabled;
    }

    // Weight of spectral direction in final score.
    void setSpectralWeight(float weight)
    {
        _spectralWeight =
            weight < 0.0f ? 0.0f : weight;
    }

    // Weight of cross-correlation direction in final score.
    void setCorrelationWeight(float weight)
    {
        _correlationWeight =
            weight < 0.0f ? 0.0f : weight;
    }

    // Physical microphone spacing in metres.
    void setMicrophoneSpacing(float metres)
    {
        if (metres > 0.0f)
            _microphoneSpacingM = metres;
    }

    // Speed of sound. 343 m/s is a reasonable starting value.
    void setSoundSpeed(float metresPerSecond)
    {
        if (metresPerSecond > 0.0f)
            _soundSpeedMps = metresPerSecond;
    }

    // Maximum integer correlation lag searched in either
    // direction. If set to 0, the physically calculated
    // maximum lag is used.
    void setMaxCorrelationLag(uint8_t samples)
    {
        _maxCorrelationLag = samples;
    }

    // Minimum normalized correlation required before the
    // correlation result is trusted.
    void setMinimumCorrelationStrength(float strength)
    {
        _minCorrelationStrength =
            clamp(strength, 0.0f, 1.0f);
    }

    // Set balance modifier member functions
    void setCenterBias(uint8_t band, float bias)
    {
        if (band < MAX_BANDS)
            _centerBias[band] = bias;
    }

    void setCenterBiases(const float* bias, uint8_t count)
    {
        if (bias == nullptr)
            return;

        if (count > MAX_BANDS)
            count = MAX_BANDS;

        for (uint8_t i = 0; i < count; ++i)
            _centerBias[i] = bias[i];
    }


    // --------------------------------------------------------
    // Reset state machine.
    // --------------------------------------------------------

    void reset()
    {
        _state = CENTER;
        _leftCandidateCount = 0;
        _rightCandidateCount = 0;
        _centerCandidateCount = 0;

        for (uint8_t i = 0; i < MAX_BANDS; ++i)
            _noiseThreshold[i] = 0.0f;
    }

    Direction direction() const
    {
        return _state;
    }

private:
    static constexpr uint8_t MAX_BANDS = 16;

    // Mathematical epsilon only.
    // It is NOT the acoustic noise threshold.
    static constexpr float DEFAULT_EPSILON = 1.0e-9f;

    Direction _state;

    uint8_t _leftCandidateCount;
    uint8_t _rightCandidateCount;
    uint8_t _centerCandidateCount;

    float _enterThreshold;
    float _leaveThreshold;

    uint8_t _directionPersistence;
    uint8_t _centerPersistence;

    bool _correlationEnabled;

    float _correlationWeight;
    float _spectralWeight;

    float _microphoneSpacingM;
    float _soundSpeedMps;

    uint8_t _maxCorrelationLag;

    float _minCorrelationStrength;

    float _epsilon;

    float _noiseThreshold[MAX_BANDS];

    float _centerBias[MAX_BANDS];

    // --------------------------------------------------------
    // Cross-correlation.
    //
    // Correlation convention:
    //
    //     C(lag) = sum(MIC1[n] * MIC2[n + lag])
    //
    // If MIC1 leads MIC2:
    //
    //     lag > 0
    //
    // Since MIC1 = RIGHT:
    //
    //     positive delay -> RIGHT
    //     negative delay -> LEFT
    // --------------------------------------------------------

    void calculateCorrelation(
        const int16_t* adcBuffer,
        uint32_t samplePairs,
        uint32_t sampleRate,
        float& peakCorrelation,
        float& directionalScore,
        float& delaySamples)
    {
        if (samplePairs < 8 || sampleRate == 0) {
            peakCorrelation = 0.0f;
            directionalScore = 0.0f;
            delaySamples = 0.0f;
            return;
        }

        // Determine physically possible maximum delay.
        float physicalDelay =
            (_microphoneSpacingM /
             _soundSpeedMps) *
            (float)sampleRate;

        uint8_t physicalLag =
            (uint8_t)ceilf(fabsf(physicalDelay));

        uint8_t searchLag = physicalLag;

        if (_maxCorrelationLag != 0 &&
            _maxCorrelationLag < searchLag) {
            searchLag = _maxCorrelationLag;
        }

        // The user may intentionally configure a search range
        // larger than the physical maximum.
        if (_maxCorrelationLag > physicalLag)
            searchLag = _maxCorrelationLag;

        if (searchLag > 12)
            searchLag = 12;

        // Avoid using the first/last few samples excessively.
        uint32_t usableStart = searchLag + 1;
        uint32_t usableEnd =
            samplePairs > searchLag + 1
                ? samplePairs - searchLag - 1
                : 0;

        if (usableEnd <= usableStart + 4) {
            peakCorrelation = 0.0f;
            directionalScore = 0.0f;
            delaySamples = 0.0f;
            return;
        }

        // ----------------------------------------------------
        // Remove DC offset from both channels.
        // ----------------------------------------------------

        float mean1 = 0.0f;
        float mean2 = 0.0f;

        uint32_t count =
            usableEnd - usableStart;

        for (uint32_t n = usableStart;
             n < usableEnd;
             ++n) {

            mean1 += (float)adcBuffer[2 * n];
            mean2 += (float)adcBuffer[2 * n + 1];
        }

        mean1 /= (float)count;
        mean2 /= (float)count;

        // ----------------------------------------------------
        // Calculate signal energies once.
        // ----------------------------------------------------

        float energy1 = 0.0f;
        float energy2 = 0.0f;

        for (uint32_t n = usableStart;
             n < usableEnd;
             ++n) {

            float x =
                (float)adcBuffer[2 * n] - mean1;

            float y =
                (float)adcBuffer[2 * n + 1] - mean2;

            energy1 += x * x;
            energy2 += y * y;
        }

        float normalization =
            sqrtf(energy1 * energy2);

        if (normalization < _epsilon) {
            peakCorrelation = 0.0f;
            directionalScore = 0.0f;
            delaySamples = 0.0f;
            return;
        }

        // ----------------------------------------------------
        // Search correlation around zero lag.
        // ----------------------------------------------------

        float bestCorrelation = -2.0f;
        int bestLag = 0;

        for (int lag = -(int)searchLag;
             lag <= (int)searchLag;
             ++lag) {

            float sum = 0.0f;

            uint32_t start = usableStart;
            uint32_t end = usableEnd;

            if (lag > 0)
                end -= (uint32_t)lag;
            else if (lag < 0)
                start += (uint32_t)(-lag);

            if (end <= start)
                continue;

            for (uint32_t n = start; n < end; ++n) {

                float x =
                    (float)adcBuffer[2 * n] - mean1;

                float y =
                    (float)adcBuffer[2 * (n + lag) + 1]
                    - mean2;

                sum += x * y;
            }

            float normalized =
                sum / (normalization + _epsilon);

            if (normalized > bestCorrelation) {
                bestCorrelation = normalized;
                bestLag = lag;
            }
        }

        if (bestCorrelation < -1.0f) {
            peakCorrelation = 0.0f;
            directionalScore = 0.0f;
            delaySamples = 0.0f;
            return;
        }

        // ----------------------------------------------------
        // Sub-sample peak interpolation.
        //
        // Use a parabola through:
        //
        //     correlation[lag-1]
        //     correlation[lag]
        //     correlation[lag+1]
        //
        // This is useful because 50 mm gives only about
        // 2.33 samples of maximum physical delay.
        // ----------------------------------------------------

        float subSampleOffset = 0.0f;

        if (bestLag > -(int)searchLag &&
            bestLag < (int)searchLag) {

            float cMinus =
                normalizedCorrelationAtLag(
                    adcBuffer,
                    samplePairs,
                    usableStart,
                    usableEnd,
                    bestLag - 1,
                    mean1,
                    mean2,
                    normalization
                );

            float cZero = bestCorrelation;

            float cPlus =
                normalizedCorrelationAtLag(
                    adcBuffer,
                    samplePairs,
                    usableStart,
                    usableEnd,
                    bestLag + 1,
                    mean1,
                    mean2,
                    normalization
                );

            float denominator =
                cMinus - 2.0f * cZero + cPlus;

            if (fabsf(denominator) > _epsilon) {
                subSampleOffset =
                    0.5f *
                    (cMinus - cPlus) /
                    denominator;

                subSampleOffset =
                    clamp(subSampleOffset, -0.5f, 0.5f);
            }
        }

        delaySamples =
            (float)bestLag + subSampleOffset;

        peakCorrelation =
            clamp(bestCorrelation, -1.0f, 1.0f);

        // ----------------------------------------------------
        // Convert delay to directional score.
        //
        // Expected maximum delay is:
        //
        //     spacing / speed_of_sound * sample_rate
        //
        // 50 mm @ 16 kHz ≈ 2.33 samples.
        // ----------------------------------------------------

        float expectedMaximumDelay =
            physicalDelay;

        if (expectedMaximumDelay < 0.1f)
            expectedMaximumDelay = 0.1f;

        directionalScore =
            delaySamples / expectedMaximumDelay;

        directionalScore =
            clamp(directionalScore, -1.0f, 1.0f);

        // If correlation itself is weak, its directional score
        // will still be reported for diagnostics, but update()
        // will not use it unless it passes the configured
        // minimum correlation strength.
    }

    float normalizedCorrelationAtLag(
        const int16_t* adcBuffer,
        uint32_t samplePairs,
        uint32_t usableStart,
        uint32_t usableEnd,
        int lag,
        float mean1,
        float mean2,
        float normalization)
    {
        if (lag < 0) {
            uint32_t shift = (uint32_t)(-lag);

            if (usableStart + shift >= usableEnd)
                return 0.0f;

            usableStart += shift;
        } else if (lag > 0) {
            if (usableEnd <= (uint32_t)lag)
                return 0.0f;

            usableEnd -= (uint32_t)lag;
        }

        if (usableEnd <= usableStart)
            return 0.0f;

        float sum = 0.0f;

        for (uint32_t n = usableStart;
             n < usableEnd;
             ++n) {

            float x =
                (float)adcBuffer[2 * n] - mean1;

            float y =
                (float)adcBuffer[2 * (n + lag) + 1]
                - mean2;

            sum += x * y;
        }

        return sum / (normalization + _epsilon);
    }

    // --------------------------------------------------------
    // State machine with hysteresis.
    //
    // LEFT:
    //     score <= -enterThreshold
    //
    // RIGHT:
    //     score >= +enterThreshold
    //
    // CENTER:
    //     region between those boundaries.
    //
    // Hysteresis:
    //
    // LEFT remains active until score rises above
    // -leaveThreshold.
    //
    // RIGHT remains active until score falls below
    // +leaveThreshold.
    // --------------------------------------------------------

    void updateState(float score)
    {
        if (_state == CENTER) {
            _centerCandidateCount = 0;

            if (score <= -_enterThreshold) {
                _leftCandidateCount++;
                _rightCandidateCount = 0;

                if (_leftCandidateCount >=
                    _directionPersistence) {

                    _state = LEFT;
                    _leftCandidateCount = 0;
                }
            } else if (score >= _enterThreshold) {
                _rightCandidateCount++;
                _leftCandidateCount = 0;

                if (_rightCandidateCount >=
                    _directionPersistence) {

                    _state = RIGHT;
                    _rightCandidateCount = 0;
                }
            } else {
                _leftCandidateCount = 0;
                _rightCandidateCount = 0;
            }

            return;
        }

        if (_state == LEFT) {
            if (score >= _enterThreshold) {
                _rightCandidateCount++;
                _centerCandidateCount = 0;

                if (_rightCandidateCount >=
                    _directionPersistence) {

                    _state = RIGHT;
                    _rightCandidateCount = 0;
                }

                return;
            }

            _rightCandidateCount = 0;

            if (score > -_leaveThreshold) {
                _centerCandidateCount++;

                if (_centerCandidateCount >=
                    _centerPersistence) {

                    _state = CENTER;
                    _centerCandidateCount = 0;
                }
            } else {
                _centerCandidateCount = 0;
            }

            return;
        }

        if (_state == RIGHT) {
            if (score <= -_enterThreshold) {
                _leftCandidateCount++;
                _centerCandidateCount = 0;

                if (_leftCandidateCount >=
                    _directionPersistence) {

                    _state = LEFT;
                    _leftCandidateCount = 0;
                }

                return;
            }

            _leftCandidateCount = 0;

            if (score < _leaveThreshold) {
                _centerCandidateCount++;

                if (_centerCandidateCount >=
                    _centerPersistence) {

                    _state = CENTER;
                    _centerCandidateCount = 0;
                }
            } else {
                _centerCandidateCount = 0;
            }
        }
    }

    Result makeResult(
        float score,
        float confidence,
        float agreement,
        uint8_t activeBands,
        float correlation,
        float delaySamples)
    {
        Result result;
        result.direction = _state;
        result.score = score;
        result.confidence = confidence;
        result.agreement = agreement;
        result.correlation = correlation;
        result.correlationScore = 0.0f;
        result.delaySamples = delaySamples;
        result.activeBands = activeBands;
        return result;
    }

    static float clamp(float value, float minimum, float maximum)
    {
        if (value < minimum) return minimum;
        if (value > maximum) return maximum;
        return value;
    }
};

#endif // SOUND_DIRECTION_HPP
