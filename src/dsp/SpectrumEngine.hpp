#pragma once

#include "Fft.hpp"
#include <vector>
#include <mutex>
#include <cstdint>
#include <chrono>

namespace RTLSDR {

enum class WaterfallPalette {
    Turbo   = 0,
    Viridis = 1,
    Plasma  = 2,
    Fire    = 3
};

struct SpectrumSnapshot {
    std::vector<float> frequenciesMHz;
    std::vector<float> currentDbfs;
    std::vector<float> maxHoldDbfs;
    std::vector<float> avgDbfs;

    double centerFreqHz = 0;
    double sampleRateHz = 0;
    float peakFreqMHz = 0.0f;
    float peakDbfs = -120.0f;
    float noiseFloorDbfs = -90.0f;
};

class SpectrumEngine {
public:
    explicit SpectrumEngine(size_t fftSize = 1024, size_t waterfallLines = 256);
    ~SpectrumEngine() = default;

    void setFftSize(size_t fftSize);
    size_t getFftSize() const { return fftSize_; }

    void setWaterfallPalette(WaterfallPalette palette) { palette_ = palette; }
    void setAveraging(float alpha) { avgAlpha_ = alpha; } // 0.0 (fast) to 0.95 (slow)
    void resetMaxHold();

    // Ingest streaming I/Q bytes from SDR
    void pushIqBytes(const uint8_t* data, size_t length, double centerFreqHz, double sampleRateHz);

    // Get latest snapshot for UI plotting
    bool getSnapshot(SpectrumSnapshot& snapshot);

    // Waterfall texture accessor: returns raw RGBA pixel data (size: fftSize x waterfallLines x 4)
    const std::vector<uint32_t>& getWaterfallPixels() const { return waterfallRgba_; }
    size_t getWaterfallWidth() const { return fftSize_; }
    size_t getWaterfallHeight() const { return waterfallLines_; }
    bool isWaterfallDirty() const { return waterfallDirty_; }
    void clearWaterfallDirty() { waterfallDirty_ = false; }

private:
    void updateWaterfallLine(const std::vector<float>& dbfsLine);
    uint32_t mapColor(float normalizedVal) const;

    size_t fftSize_ = 1024;
    size_t waterfallLines_ = 256;
    WaterfallPalette palette_ = WaterfallPalette::Turbo;
    float avgAlpha_ = 0.65f;

    FastFft fft_;
    std::vector<uint8_t> sampleAccumulator_;

    mutable std::mutex dataMutex_;
    SpectrumSnapshot snapshot_;
    std::vector<float> maxHold_;
    std::vector<float> avgTrace_;

    // 2D Waterfall RGBA pixel buffer
    std::vector<uint32_t> waterfallRgba_;
    size_t waterfallHead_ = 0;
    bool waterfallDirty_ = false;
};

} // namespace RTLSDR
