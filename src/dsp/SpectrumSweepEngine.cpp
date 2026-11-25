#include "SpectrumSweepEngine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace RTLSDR {

SpectrumSweepEngine::SpectrumSweepEngine(size_t displayBins, size_t waterfallLines)
    : displayBins_(displayBins)
    , waterfallLines_(waterfallLines)
    , fft_(1024)
{
    lastTuneTime_ = std::chrono::steady_clock::now();
    rebuildGrid();
}

void SpectrumSweepEngine::rebuildGrid() {
    if (config_.stopFreqMHz <= config_.startFreqMHz) {
        config_.stopFreqMHz = config_.startFreqMHz + 1.0;
    }

    gridFreqsMHz_.resize(displayBins_);
    const double span = config_.stopFreqMHz - config_.startFreqMHz;
    for (size_t i = 0; i < displayBins_; ++i) {
        gridFreqsMHz_[i] = static_cast<float>(
            config_.startFreqMHz + (static_cast<double>(i) / static_cast<double>(displayBins_ - 1)) * span
        );
    }

    currentDbfs_.assign(displayBins_, -105.0f);
    maxHoldDbfs_.assign(displayBins_, -105.0f);
    waterfallRgba_.assign(displayBins_ * waterfallLines_, 0xFF000000);
    waterfallDirty_ = true;
}

void SpectrumSweepEngine::configure(const SweepConfig& cfg) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool rangeChanged = (std::abs(cfg.startFreqMHz - config_.startFreqMHz) > 1e-4) ||
                        (std::abs(cfg.stopFreqMHz - config_.stopFreqMHz) > 1e-4);
    bool fftChanged = (cfg.fftSize != config_.fftSize);

    config_ = cfg;
    if (fftChanged) {
        fft_.setSize(config_.fftSize);
        sampleAccumulator_.clear();
    }
    if (rangeChanged) {
        sweepCount_ = 0;
        currentStepFreqHz_ = config_.startFreqMHz * 1e6;
        rebuildGrid();
    }
}

SweepConfig SpectrumSweepEngine::getConfig() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void SpectrumSweepEngine::startSweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.active = true;
    currentStepFreqHz_ = config_.startFreqMHz * 1e6;
    stepCaptured_ = false;
    discardBytesRemaining_ = 24576;
    sampleAccumulator_.clear();
    lastTuneTime_ = std::chrono::steady_clock::now() - std::chrono::milliseconds(config_.dwellTimeMs + 10);
}

void SpectrumSweepEngine::stopSweep() {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.active = false;
}

bool SpectrumSweepEngine::isActive() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_.active;
}

void SpectrumSweepEngine::resetMaxHold() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::fill(maxHoldDbfs_.begin(), maxHoldDbfs_.end(), -105.0f);
}

void SpectrumSweepEngine::setWaterfallPalette(WaterfallPalette palette) {
    std::lock_guard<std::mutex> lock(mutex_);
    palette_ = palette;
}

bool SpectrumSweepEngine::pollNextTuneFreq(double sampleRateHz, uint32_t& outFreqHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.active || sampleRateHz <= 0.0) return false;

    auto now = std::chrono::steady_clock::now();
    auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime_).count();

    const int safetyTimeoutMs = std::max(config_.dwellTimeMs * 3, 250);
    if (!(stepCaptured_ && elapsedMs >= config_.dwellTimeMs) && elapsedMs < safetyTimeoutMs) {
        return false;
    }

    const double stepHz = std::max(250000.0, sampleRateHz * 0.72);
    const double startHz = config_.startFreqMHz * 1e6;
    const double stopHz = config_.stopFreqMHz * 1e6;

    double nextFreq = currentStepFreqHz_;
    if (stepCaptured_ || elapsedMs >= safetyTimeoutMs) {
        nextFreq += stepHz;
        if (nextFreq - stepHz * 0.35 > stopHz) {
            // Completed full sweep pass
            updateWaterfallRow();
            sweepCount_++;
            nextFreq = startHz + stepHz * 0.5;
            if (nextFreq > stopHz) {
                nextFreq = 0.5 * (startHz + stopHz);
            }
        }
    }

    currentStepFreqHz_ = nextFreq;
    outFreqHz = static_cast<uint32_t>(std::round(nextFreq));
    return true;
}

void SpectrumSweepEngine::notifyTuned(uint32_t freqHz) {
    std::lock_guard<std::mutex> lock(mutex_);
    currentStepFreqHz_ = static_cast<double>(freqHz);
    discardBytesRemaining_ = 24576; // Discard ~12k I/Q pairs after retune
    sampleAccumulator_.clear();
    stepCaptured_ = false;
    lastTuneTime_ = std::chrono::steady_clock::now();
}

void SpectrumSweepEngine::pushIqBytes(const uint8_t* data, size_t length, double centerFreqHz, double sampleRateHz) {
    if (!data || length == 0 || sampleRateHz <= 0.0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.active) return;

    // Discard transient samples right after retuning
    if (discardBytesRemaining_ > 0) {
        size_t toDiscard = std::min(length, discardBytesRemaining_);
        discardBytesRemaining_ -= toDiscard;
        data += toDiscard;
        length -= toDiscard;
        if (length == 0) return;
    }

    sampleAccumulator_.insert(sampleAccumulator_.end(), data, data + length);

    const size_t fftBytes = config_.fftSize * 2;
    if (sampleAccumulator_.size() < fftBytes) return;

    std::vector<float> fftOut;
    fft_.processIqBytes(sampleAccumulator_.data(), config_.fftSize, fftOut);
    sampleAccumulator_.clear();

    // Stitch central 75% of FFT bins into the wideband display grid
    const size_t margin = config_.fftSize / 8;
    const size_t validStart = margin;
    const size_t validEnd = config_.fftSize - margin;

    const double startMHz = config_.startFreqMHz;
    const double spanMHz = config_.stopFreqMHz - config_.startFreqMHz;

    std::vector<bool> binTouched(displayBins_, false);

    for (size_t b = validStart; b < validEnd; ++b) {
        // Skip center DC bin +/- 1
        if (b >= config_.fftSize / 2 - 1 && b <= config_.fftSize / 2 + 1) continue;

        double norm = (static_cast<double>(b) / static_cast<double>(config_.fftSize)) - 0.5;
        double binFreqMHz = (centerFreqHz + norm * sampleRateHz) / 1e6;

        if (binFreqMHz < startMHz || binFreqMHz > config_.stopFreqMHz) continue;

        size_t dispIdx = static_cast<size_t>(
            ((binFreqMHz - startMHz) / spanMHz) * static_cast<double>(displayBins_ - 1) + 0.5
        );
        if (dispIdx >= displayBins_) continue;

        float val = fftOut[b];
        if (!binTouched[dispIdx]) {
            currentDbfs_[dispIdx] = val;
            binTouched[dispIdx] = true;
        } else {
            currentDbfs_[dispIdx] = std::max(currentDbfs_[dispIdx], val);
        }

        if (currentDbfs_[dispIdx] > maxHoldDbfs_[dispIdx]) {
            maxHoldDbfs_[dispIdx] = currentDbfs_[dispIdx];
        }
    }

    stepCaptured_ = true;
}

uint32_t SpectrumSweepEngine::mapColor(float t) const {
    t = std::max(0.0f, std::min(1.0f, t));
    uint8_t r = 0, g = 0, b = 0;

    switch (palette_) {
        case WaterfallPalette::Fire: {
            if (t < 0.33f) {
                r = static_cast<uint8_t>((t / 0.33f) * 255);
            } else if (t < 0.66f) {
                r = 255;
                g = static_cast<uint8_t>(((t - 0.33f) / 0.33f) * 255);
            } else {
                r = 255;
                g = 255;
                b = static_cast<uint8_t>(((t - 0.66f) / 0.34f) * 255);
            }
            break;
        }
        case WaterfallPalette::Viridis: {
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
            if (t < 0.25f) {
                b = 255;
                g = static_cast<uint8_t>((t / 0.25f) * 255);
            } else if (t < 0.50f) {
                g = 255;
                b = static_cast<uint8_t>((1.0f - (t - 0.25f) / 0.25f) * 255);
            } else if (t < 0.75f) {
                g = 255;
                r = static_cast<uint8_t>(((t - 0.50f) / 0.25f) * 255);
            } else {
                r = 255;
                g = static_cast<uint8_t>((1.0f - (t - 0.75f) / 0.25f) * 255);
            }
            break;
        }
    }

    return (0xFF000000) | (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(r);
}

void SpectrumSweepEngine::updateWaterfallRow() {
    if (waterfallLines_ > 1) {
        std::memmove(
            waterfallRgba_.data() + displayBins_,
            waterfallRgba_.data(),
            (waterfallLines_ - 1) * displayBins_ * sizeof(uint32_t)
        );
    }

    const float minDb = -95.0f;
    const float maxDb = -15.0f;
    const float range = maxDb - minDb;

    for (size_t x = 0; x < displayBins_; ++x) {
        float val = (currentDbfs_[x] - minDb) / range;
        waterfallRgba_[x] = mapColor(val);
    }
    waterfallDirty_ = true;
}

bool SpectrumSweepEngine::getSnapshot(SweepSnapshot& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (gridFreqsMHz_.empty()) return false;

    out.frequenciesMHz = gridFreqsMHz_;
    out.currentDbfs = currentDbfs_;
    out.maxHoldDbfs = maxHoldDbfs_;
    out.sweepCount = sweepCount_;
    out.currentScanFreqMHz = currentStepFreqHz_ / 1e6;

    double spanHz = (config_.stopFreqMHz - config_.startFreqMHz) * 1e6;
    if (spanHz > 0.0) {
        out.progress = static_cast<float>(
            std::max(0.0, std::min(1.0, (currentStepFreqHz_ - config_.startFreqMHz * 1e6) / spanHz))
        );
    } else {
        out.progress = 0.0f;
    }

    float maxVal = -150.0f;
    float maxFreq = gridFreqsMHz_[0];
    for (size_t i = 0; i < currentDbfs_.size(); ++i) {
        if (currentDbfs_[i] > maxVal) {
            maxVal = currentDbfs_[i];
            maxFreq = gridFreqsMHz_[i];
        }
    }
    out.peakFreqMHz = maxFreq;
    out.peakDbfs = maxVal;

    return true;
}

} // namespace RTLSDR
