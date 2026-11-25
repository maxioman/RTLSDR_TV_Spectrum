#pragma once

#include "Fft.hpp"
#include "SpectrumEngine.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace RTLSDR {

struct SweepConfig {
    double startFreqMHz = 88.0;
    double stopFreqMHz  = 108.0;
    int dwellTimeMs     = 65;
    size_t fftSize      = 1024;
    bool active         = false;
};

struct SweepSnapshot {
    std::vector<float> frequenciesMHz;
    std::vector<float> currentDbfs;
    std::vector<float> maxHoldDbfs;

    float peakFreqMHz = 0.0f;
    float peakDbfs = -120.0f;
    float progress = 0.0f;
    double currentScanFreqMHz = 0.0;
    uint32_t sweepCount = 0;
};

class SpectrumSweepEngine {
public:
    explicit SpectrumSweepEngine(size_t displayBins = 1024, size_t waterfallLines = 180);
    ~SpectrumSweepEngine() = default;

    void configure(const SweepConfig& cfg);
    SweepConfig getConfig() const;

    void startSweep();
    void stopSweep();
    bool isActive() const;

    void resetMaxHold();
    void setWaterfallPalette(WaterfallPalette palette);

    // Returns true if the sweep engine wants to retune the SDR to outFreqHz
    bool pollNextTuneFreq(double sampleRateHz, uint32_t& outFreqHz);
    void notifyTuned(uint32_t freqHz);

    // Ingest streaming I/Q bytes from SDR
    void pushIqBytes(const uint8_t* data, size_t length, double centerFreqHz, double sampleRateHz);

    bool getSnapshot(SweepSnapshot& out) const;

    const std::vector<uint32_t>& getWaterfallPixels() const { return waterfallRgba_; }
    size_t getWaterfallWidth() const { return displayBins_; }
    size_t getWaterfallHeight() const { return waterfallLines_; }
    bool isWaterfallDirty() const { return waterfallDirty_; }
    void clearWaterfallDirty() { waterfallDirty_ = false; }

private:
    void rebuildGrid();
    void updateWaterfallRow();
    uint32_t mapColor(float normalizedVal) const;

    SweepConfig config_;
    size_t displayBins_ = 1024;
    size_t waterfallLines_ = 180;
    WaterfallPalette palette_ = WaterfallPalette::Turbo;

    FastFft fft_;
    std::vector<uint8_t> sampleAccumulator_;

    mutable std::mutex mutex_;
    std::vector<float> gridFreqsMHz_;
    std::vector<float> currentDbfs_;
    std::vector<float> maxHoldDbfs_;
    std::vector<uint32_t> waterfallRgba_;
    bool waterfallDirty_ = false;

    // Sweep state machine
    double currentStepFreqHz_ = 88.0e6;
    size_t discardBytesRemaining_ = 0;
    bool stepCaptured_ = false;
    uint32_t sweepCount_ = 0;
    std::chrono::steady_clock::time_point lastTuneTime_;
};

} // namespace RTLSDR
