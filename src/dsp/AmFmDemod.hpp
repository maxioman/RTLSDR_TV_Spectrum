#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace RTLSDR {

enum class DemodMode {
    AM  = 0,
    NFM = 1,
    WFM = 2
};

struct AmFmConfig {
    DemodMode mode = DemodMode::WFM;
    double channelOffsetHz = 0.0;   // Offset from SDR center frequency
    float bandwidthHz = 180000.0f;  // IF channel bandwidth
    float squelchDbfs = -85.0f;     // Squelch threshold in dBFS
    float volume = 0.6f;            // Output gain [0.0 .. 1.0]
    float deemphasisUs = 50.0f;     // 0 (off), 50 us (EU), or 75 us (US)
    bool audioEnabled = false;
};

struct AmFmStats {
    float signalRssiDbfs = -120.0f;
    bool squelchOpen = false;
    double tunedFreqMHz = 0.0;
};

class AmFmDemod {
public:
    AmFmDemod();
    ~AmFmDemod() = default;

    void setConfig(const AmFmConfig& cfg);
    AmFmConfig getConfig() const;

    void processIqBytes(const uint8_t* iqData, size_t length, double centerFreqHz, double sampleRateHz);

    size_t getAudioSamples(float* dest, size_t maxSamples);
    void getWaveform(std::vector<float>& outWaveform) const;
    AmFmStats getStats() const;
    void reset();

private:
    AmFmConfig config_;
    mutable std::mutex configMutex_;

    // Frequency-shift NCO
    double ncoPhase_ = 0.0;

    // 2-stage cascaded complex IIR low-pass filter
    float lpf1I_ = 0.0f, lpf1Q_ = 0.0f;
    float lpf2I_ = 0.0f, lpf2Q_ = 0.0f;

    // IF decimation & demodulator state
    double ifSampleAccum_ = 0.0;
    float prevIfI_ = 0.0f;
    float prevIfQ_ = 0.0f;

    // AM DC-blocker & AGC
    float amDcPrevIn_ = 0.0f;
    float amDcPrevOut_ = 0.0f;
    float amAgcEnv_ = 0.1f;

    // FM de-emphasis & audio decimation
    float deemphState_ = 0.0f;
    double audioAccum_ = 0.0;
    float audioSum_ = 0.0f;
    int audioCount_ = 0;

    // RSSI & Squelch
    float powerAvg_ = 1e-10f;
    float squelchGain_ = 0.0f;

    // Audio ring buffer (48 kHz mono float)
    std::vector<float> audioRingBuffer_;
    size_t audioWriteIdx_ = 0;
    size_t audioReadIdx_ = 0;
    mutable std::mutex audioMutex_;

    // Live AF waveform for oscilloscope
    std::vector<float> waveformBuffer_;
    size_t waveformWriteIdx_ = 0;
    mutable std::mutex waveformMutex_;

    AmFmStats stats_;
    mutable std::mutex statsMutex_;
};

} // namespace RTLSDR
