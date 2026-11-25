#include "SpectrumEngine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace RTLSDR {

SpectrumEngine::SpectrumEngine(size_t fftSize, size_t waterfallLines)
    : fftSize_(fftSize), waterfallLines_(waterfallLines), fft_(fftSize)
{
    maxHold_.assign(fftSize_, -120.0f);
    avgTrace_.assign(fftSize_, -90.0f);
    waterfallRgba_.assign(fftSize_ * waterfallLines_, 0xFF000000); // Opaque black
}

void SpectrumEngine::setFftSize(size_t fftSize) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    fftSize_ = fftSize;
    fft_.setSize(fftSize);
    maxHold_.assign(fftSize_, -120.0f);
    avgTrace_.assign(fftSize_, -90.0f);
    waterfallRgba_.assign(fftSize_ * waterfallLines_, 0xFF000000);
    sampleAccumulator_.clear();
}

void SpectrumEngine::resetMaxHold() {
    std::lock_guard<std::mutex> lock(dataMutex_);
    maxHold_.assign(fftSize_, -120.0f);
}

uint32_t SpectrumEngine::mapColor(float t) const {
    t = std::max(0.0f, std::min(1.0f, t));
    uint8_t r = 0, g = 0, b = 0;

    switch (palette_) {
        case WaterfallPalette::Fire: {
            // Black -> Red -> Orange -> Yellow -> White
            if (t < 0.33f) {
                float u = t / 0.33f;
                r = static_cast<uint8_t>(u * 255);
            } else if (t < 0.66f) {
                float u = (t - 0.33f) / 0.33f;
                r = 255;
                g = static_cast<uint8_t>(u * 255);
            } else {
                float u = (t - 0.66f) / 0.34f;
                r = 255;
                g = 255;
                b = static_cast<uint8_t>(u * 255);
            }
            break;
        }
        case WaterfallPalette::Viridis: {
            // Dark purple -> Teal -> Green -> Yellow
            r = static_cast<uint8_t>((0.28f + 0.72f * t * t) * 255);
            g = static_cast<uint8_t>(std::sin(t * static_cast<float>(M_PI)) * 255);
            b = static_cast<uint8_t>((1.0f - t) * (0.5f + 0.5f * (1.0f - t)) * 255);
            break;
        }
        case WaterfallPalette::Plasma: {
            r = static_cast<uint8_t>((0.05f + 0.95f * t) * 255);
            g = static_cast<uint8_t>((0.02f + 0.85f * std::sin(t * static_cast<float>(M_PI))) * 255);
            b = static_cast<uint8_t>((0.5f + 0.5f * std::cos(t * static_cast<float>(M_PI))) * 255);
            break;
        }
        case WaterfallPalette::Turbo:
        default: {
            // Classic scientific rainbow: Dark Blue -> Cyan -> Green -> Yellow -> Red
            if (t < 0.25f) {
                float u = t / 0.25f;
                b = 255;
                g = static_cast<uint8_t>(u * 255);
            } else if (t < 0.50f) {
                float u = (t - 0.25f) / 0.25f;
                g = 255;
                b = static_cast<uint8_t>((1.0f - u) * 255);
            } else if (t < 0.75f) {
                float u = (t - 0.50f) / 0.25f;
                g = 255;
                r = static_cast<uint8_t>(u * 255);
            } else {
                float u = (t - 0.75f) / 0.25f;
                r = 255;
                g = static_cast<uint8_t>((1.0f - u) * 255);
            }
            break;
        }
    }

    // Return 32-bit RGBA (0xAABBGGRR in little-endian format)
    return (0xFF000000) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

void SpectrumEngine::updateWaterfallLine(const std::vector<float>& dbfsLine) {
    // Scroll waterfall down by 1 row
    size_t rowPixels = fftSize_;
    if (waterfallLines_ > 1) {
        std::memmove(
            waterfallRgba_.data() + rowPixels,
            waterfallRgba_.data(),
            (waterfallLines_ - 1) * rowPixels * sizeof(uint32_t)
        );
    }

    // Write new row at top (index 0..fftSize_-1)
    const float minDb = -85.0f;
    const float maxDb = -20.0f;
    const float range = maxDb - minDb;

    for (size_t x = 0; x < fftSize_; ++x) {
        float val = (dbfsLine[x] - minDb) / range;
        waterfallRgba_[x] = mapColor(val);
    }
    waterfallDirty_ = true;
}

void SpectrumEngine::pushIqBytes(const uint8_t* data, size_t length, double centerFreqHz, double sampleRateHz) {
    if (!data || length == 0) return;

    sampleAccumulator_.insert(sampleAccumulator_.end(), data, data + length);

    size_t bytesNeeded = fftSize_ * 2; // I and Q bytes
    if (sampleAccumulator_.size() < bytesNeeded) return;

    // Grab the latest fftSize_ samples from the end of the accumulator
    size_t startOffset = sampleAccumulator_.size() - bytesNeeded;
    const uint8_t* slice = sampleAccumulator_.data() + startOffset;

    std::vector<float> dbfs;
    fft_.processIqBytes(slice, fftSize_, dbfs);

    // Keep accumulator bounded
    sampleAccumulator_.clear();

    // Update state under lock
    std::lock_guard<std::mutex> lock(dataMutex_);

    if (snapshot_.frequenciesMHz.size() != fftSize_) {
        snapshot_.frequenciesMHz.resize(fftSize_);
    }

    double halfSpan = sampleRateHz / 2.0;
    double startFreq = centerFreqHz - halfSpan;
    double stepFreq = sampleRateHz / static_cast<double>(fftSize_);

    float peakVal = -150.0f;
    float peakF = static_cast<float>(centerFreqHz / 1e6);
    float noiseSum = 0.0f;

    for (size_t i = 0; i < fftSize_; ++i) {
        float fMHz = static_cast<float>((startFreq + i * stepFreq) / 1e6);
        snapshot_.frequenciesMHz[i] = fMHz;

        float curDbfs = dbfs[i];
        maxHold_[i] = std::max(maxHold_[i], curDbfs);
        avgTrace_[i] = avgAlpha_ * avgTrace_[i] + (1.0f - avgAlpha_) * curDbfs;

        if (curDbfs > peakVal) {
            peakVal = curDbfs;
            peakF = fMHz;
        }
        noiseSum += curDbfs;
    }

    snapshot_.currentDbfs = dbfs;
    snapshot_.maxHoldDbfs = maxHold_;
    snapshot_.avgDbfs = avgTrace_;
    snapshot_.centerFreqHz = centerFreqHz;
    snapshot_.sampleRateHz = sampleRateHz;
    snapshot_.peakFreqMHz = peakF;
    snapshot_.peakDbfs = peakVal;
    snapshot_.noiseFloorDbfs = noiseSum / static_cast<float>(fftSize_);

    updateWaterfallLine(dbfs);
}

bool SpectrumEngine::getSnapshot(SpectrumSnapshot& snapshot) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (snapshot_.currentDbfs.empty()) return false;
    snapshot = snapshot_;
    return true;
}

} // namespace RTLSDR
