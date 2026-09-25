#ifndef SAMPLER_H
#define SAMPLER_H

#include <Arduino.h>
#include <stdint.h>

class Sampler {
public:

    /*
     * sampleRate:
     *     samples per second PER CHANNEL.
     *
     * bufferSize:
     *     number of samples per channel.
     *
     * dmaBuffer:
     *     contiguous buffer containing:
     *
     *       A6[0], A7[0],
     *       A6[1], A7[1],
     *       ...
     *
     *     Size must therefore be:
     *
     *       2 * bufferSize
     *
     *     int16_t elements.
     *
     * bufferFullCallback:
     *     called from poll() after the DMA buffer
     *     is complete.
     */
    Sampler(
        uint32_t sampleRate,
        uint32_t bufferSize,
        int16_t* dmaBuffer,
        void (*bufferFullCallback)()
    );

    Sampler();

    ~Sampler();

    /*
     * Start/restart acquisition.
     */
    void start();

    /*
     * Stop acquisition.
     */
    void stop();

    /*
     * Must be called regularly from loop().
     *
     * Detects completion of the DMA transfer and
     * invokes bufferFullCallback().
     */
    void poll();

    /*
     * True while acquisition is active.
     */
    bool isRunning() const;

    /*
     * Number of sample pairs acquired.
     */
    uint32_t sampleCount() const;

    /*
     * Returns the DMA buffer.
     */
    int16_t* buffer();

private:

    void configureADC();
    void configureTimer();
    void disableHardware();

    uint32_t _sampleRate;
    uint32_t _bufferSize;

    volatile uint32_t _sampleCount;

    int16_t* _dmaBuffer;

    void (*_bufferFullCallback)();

    volatile bool _running;
    volatile bool _callbackPending;

    static Sampler* _instance;
};

#endif
