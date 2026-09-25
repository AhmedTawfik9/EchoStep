#include "Sampler.h"

#include <nrf.h>


/*
 * ------------------------------------------------------------
 * Peripheral pointers
 * ------------------------------------------------------------
 *
 * We explicitly use the nRF52840 peripheral base addresses.
 *
 * This avoids the NRF_SAADC macro/type issues encountered
 * with the Arduino Mbed build.
 */

static NRF_SAADC_Type* const saadc =
    reinterpret_cast<NRF_SAADC_Type*>(NRF_SAADC_BASE);

static NRF_TIMER_Type* const timer4 =
    reinterpret_cast<NRF_TIMER_Type*>(NRF_TIMER4_BASE);

static NRF_PPI_Type* const ppi =
    reinterpret_cast<NRF_PPI_Type*>(NRF_PPI_BASE);


/*
 * Use PPI channel 7.
 *
 * This is the channel used by the known Nano 33 BLE
 * TIMER -> PPI -> SAADC examples.
 */
static constexpr uint32_t PPI_CHANNEL = 7;


/*
 * Single Sampler instance.
 */
Sampler* Sampler::_instance = nullptr;


// ============================================================
// Constructor
// ============================================================

Sampler::Sampler(
    uint32_t sampleRate,
    uint32_t bufferSize,
    int16_t* dmaBuffer,
    void (*bufferFullCallback)()
)
    : _sampleRate(sampleRate),
      _bufferSize(bufferSize),
      _sampleCount(0),
      _dmaBuffer(dmaBuffer),
      _bufferFullCallback(bufferFullCallback),
      _running(false),
      _callbackPending(false)
{
    _instance = this;
}


// ============================================================
// Default constructor
// ============================================================

Sampler::Sampler()
    : _sampleRate(0),
      _bufferSize(0),
      _sampleCount(0),
      _dmaBuffer(nullptr),
      _bufferFullCallback(nullptr),
      _running(false),
      _callbackPending(false)
{
    _instance = this;
}


// ============================================================
// Destructor
// ============================================================

Sampler::~Sampler()
{
    stop();

    if (_instance == this) {
        _instance = nullptr;
    }
}


// ============================================================
// Configure SAADC
// ============================================================

void Sampler::configureADC()
{
    /*
     * Disable SAADC before configuring it.
     */
    saadc->ENABLE = 0;


    /*
     * Clear events.
     */
    saadc->EVENTS_STARTED = 0;
    saadc->EVENTS_END = 0;
    saadc->EVENTS_DONE = 0;
    saadc->EVENTS_STOPPED = 0;


    /*
     * --------------------------------------------------------
     * CHANNEL 0 = A6
     *
     * Nano 33 BLE:
     *
     * A6 = P0.28 = AIN4
     * --------------------------------------------------------
     */
    saadc->CH[0].PSELP =
        SAADC_CH_PSELP_PSELP_AnalogInput4;

    saadc->CH[0].PSELN =
        SAADC_CH_PSELN_PSELN_NC;


    /*
     * --------------------------------------------------------
     * CHANNEL 1 = A7
     *
     * Nano 33 BLE:
     *
     * A7 = P0.03 = AIN1
     * --------------------------------------------------------
     */
    saadc->CH[1].PSELP =
        SAADC_CH_PSELP_PSELP_AnalogInput1;

    saadc->CH[1].PSELN =
        SAADC_CH_PSELN_PSELN_NC;


    /*
     * Same ADC configuration as Arduino's default
     * analog configuration:
     *
     * reference = VDD/4
     * gain      = 1/4
     * acquisition = 10 us
     * single-ended
     * burst disabled
     */
    const uint32_t channelConfig =
        SAADC_CH_CONFIG_RESP_Bypass |
        SAADC_CH_CONFIG_RESN_Bypass |
        SAADC_CH_CONFIG_GAIN_Gain1_4 |
        SAADC_CH_CONFIG_REFSEL_VDD1_4 |
        SAADC_CH_CONFIG_TACQ_10us |
        SAADC_CH_CONFIG_MODE_SE |
        SAADC_CH_CONFIG_BURST_Disabled;


    saadc->CH[0].CONFIG = channelConfig;
    saadc->CH[1].CONFIG = channelConfig;


    /*
     * 12-bit conversion.
     */
    saadc->RESOLUTION =
        SAADC_RESOLUTION_VAL_12bit;


    /*
     * No oversampling.
     */
    saadc->OVERSAMPLE =
        SAADC_OVERSAMPLE_OVERSAMPLE_Bypass;


    /*
     * --------------------------------------------------------
     * EasyDMA
     * --------------------------------------------------------
     *
     * Two enabled channels produce:
     *
     *     A6[0]
     *     A7[0]
     *     A6[1]
     *     A7[1]
     *     ...
     */
    saadc->RESULT.PTR =
        reinterpret_cast<uint32_t>(_dmaBuffer);

    saadc->RESULT.MAXCNT =
        _bufferSize * 2;


    /*
     * We deliberately DO NOT enable the SAADC interrupt.
     *
     * Mbed already owns SAADC_IRQHandler().
     *
     * Instead, poll() checks EVENTS_END.
     */


    /*
     * Enable SAADC.
     */
    saadc->ENABLE =
        SAADC_ENABLE_ENABLE_Enabled;
}


// ============================================================
// Configure TIMER4
// ============================================================

void Sampler::configureTimer()
{
    /*
     * TIMER4 runs from 16 MHz with PRESCALER = 0.
     *
     * For 16 kHz:
     *
     *     16,000,000 / 16,000 = 1000
     *
     * Therefore:
     *
     *     CC[0] = 1000
     *
     * gives exactly 62.5 us between SAMPLE triggers.
     */
    const uint32_t ticks =
        16000000UL / _sampleRate;


    timer4->TASKS_STOP = 1;
    timer4->TASKS_CLEAR = 1;


    timer4->MODE =
        TIMER_MODE_MODE_Timer;

    timer4->BITMODE =
        TIMER_BITMODE_BITMODE_16Bit;

    timer4->PRESCALER = 0;

    timer4->CC[0] = ticks;


    /*
     * Automatically clear timer at COMPARE0.
     */
    timer4->SHORTS =
        TIMER_SHORTS_COMPARE0_CLEAR_Msk;

    timer4->EVENTS_COMPARE[0] = 0;


    /*
     * --------------------------------------------------------
     * PPI:
     *
     * TIMER4 COMPARE0
     *       |
     *       v
     * SAADC TASKS_SAMPLE
     *
     * This occurs entirely in hardware.
     */
    ppi->CH[PPI_CHANNEL].EEP =
        reinterpret_cast<uint32_t>(
            &timer4->EVENTS_COMPARE[0]
        );

    ppi->CH[PPI_CHANNEL].TEP =
        reinterpret_cast<uint32_t>(
            &saadc->TASKS_SAMPLE
        );


    /*
     * Enable PPI channel.
     */
    ppi->CHENSET =
        (1UL << PPI_CHANNEL);
}


// ============================================================
// Start
// ============================================================

void Sampler::start()
{
    if (_sampleRate == 0 ||
        _bufferSize == 0 ||
        _dmaBuffer == nullptr) {
        return;
    }


    /*
     * TIMER clock is 16 MHz.
     *
     * Require an exact divisor.
     *
     * 16 kHz is exact.
     */
    if ((16000000UL % _sampleRate) != 0) {
        return;
    }


    /*
     * Stop previous acquisition.
     */
    stop();


    _sampleCount = 0;
    _callbackPending = false;


    /*
     * Clear old SAADC END event.
     */
    saadc->EVENTS_END = 0;


    /*
     * Configure ADC.
     */
    configureADC();


    /*
     * Configure timer + PPI.
     */
    configureTimer();


    _running = true;


    /*
     * Prepare SAADC.
     */
    saadc->TASKS_START = 1;


    /*
     * Start timer.
     *
     * From this point onward the CPU does not participate
     * in individual ADC conversions.
     */
    timer4->TASKS_CLEAR = 1;
    timer4->TASKS_START = 1;
}


// ============================================================
// Stop
// ============================================================

void Sampler::stop()
{
    _running = false;

    disableHardware();
}


// ============================================================
// Disable hardware
// ============================================================

void Sampler::disableHardware()
{
    // Stop TIMER4.
    timer4->TASKS_STOP = 1;
    // Disable PPI channel.
    ppi->CHENCLR =
        (1UL << PPI_CHANNEL);

    // Stop SAADC.
    if (saadc->ENABLE != 0) {
        saadc->TASKS_STOP = 1;
    }
}


// ============================================================
// Poll
// ============================================================

void Sampler::poll()
{
    // Nothing to do if we're not currently sampling.
    if (!_running) {
        return;
    }

    // EasyDMA has completed when EVENTS_END is set.
    if (saadc->EVENTS_END == 0) {
        return;
    }

    // Clear END event.
    saadc->EVENTS_END = 0;

    // Stop timer immediately.
    // This prevents further SAMPLE triggers
    timer4->TASKS_STOP = 1;

    // Disable PPI.
    ppi->CHENCLR =
        (1UL << PPI_CHANNEL);

    // Complete acquisition.
    _sampleCount = _bufferSize;

    _running = false;

    //Stop SAADC.
    saadc->TASKS_STOP = 1;


    //Schedule callback.
    // The callback itself executes here from loop(),
    // NOT from an interrupt.
    _callbackPending = true;

    if (_callbackPending) {
        _callbackPending = false;

        if (_bufferFullCallback != nullptr) {
            _bufferFullCallback();
        }
    }
}


// ============================================================
// Status
// ============================================================

bool Sampler::isRunning() const
{
    return _running;
}


// ============================================================
// Sample count
// ============================================================

uint32_t Sampler::sampleCount() const
{
    return _sampleCount;
}


// ============================================================
// Buffer
// ============================================================

int16_t* Sampler::buffer()
{
    return _dmaBuffer;
}
