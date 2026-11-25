#include "AnalogTvDemod.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>

namespace RTLSDR {

AnalogTvDemod::AnalogTvDemod() {
    audioRingBuffer_.resize(48000, 0.0f);
    reconfigure();
}

void AnalogTvDemod::setConfig(const TvConfig& cfg) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    bool standardChanged = (cfg.standard != config_.standard);
    config_ = cfg;
    if (standardChanged) {
        reconfigure();
    }
}

TvConfig AnalogTvDemod::getConfig() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    return config_;
}

void AnalogTvDemod::setSampleRate(double sampleRateHz) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    if (std::abs(sampleRateHz_ - sampleRateHz) > 100.0) {
        sampleRateHz_ = sampleRateHz;
        reconfigure();
    }
}

void AnalogTvDemod::reconfigure() {
    switch (config_.standard) {
        case TvStandard::NTSC_M:
            lineFrequency_ = 15734.264;
            lineInFrame_ = 525;
            soundSubcarrierHz_ = 4.5e6;
            break;
        case TvStandard::PAL_I:
            lineFrequency_ = 15625.0;
            lineInFrame_ = 625;
            soundSubcarrierHz_ = 6.0e6;
            break;
        case TvStandard::PAL_D_K:
            lineFrequency_ = 15625.0;
            lineInFrame_ = 625;
            soundSubcarrierHz_ = 6.5e6;
            break;
        case TvStandard::SECAM_L:
            lineFrequency_ = 15625.0;
            lineInFrame_ = 625;
            soundSubcarrierHz_ = 6.5e6;
            config_.inverseVideo = true; // Positive video modulation
            break;
        case TvStandard::PAL_B_G:
        default:
            lineFrequency_ = 15625.0;
            lineInFrame_ = 625;
            soundSubcarrierHz_ = 5.5e6;
            break;
    }

    pictureHeight_ = (config_.standard == TvStandard::NTSC_M) ? 480 : 576;
    pictureWidth_ = 720; // Broadcast D1 raster width

    size_t totalPixels = static_cast<size_t>(pictureWidth_ * pictureHeight_);
    lumaBuffer_.assign(totalPixels, 16);
    frontRgbaBuffer_.assign(totalPixels, 0xFF000000);
    backRgbaBuffer_.assign(totalPixels, 0xFF000000);
    latestScanline_.assign(256, 0.0f);

    linePhase_ = 0.0;
    pllPhaseError_ = 0.0;
    pllFreqCorr_ = 0.0;
    fieldLine_ = 0;
    isField2_ = false;
    syncPulseSamples_ = 0;
    fieldIndex_ = 0;
    prevEffX_ = 0;
    linesSinceVSync_ = 0;
    vbiCountdown_ = 0;
    syncMagFilter_ = 0.0f;
    syncMagFilterPrev_ = 0.0f;
    isProgressive_ = false;
    consecutiveHsync_ = 0;
}

void AnalogTvDemod::fineFrequencyTick() {
    // Handled in fractional phase accumulator
}

void AnalogTvDemod::commitFrame() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    size_t total = static_cast<size_t>(pictureWidth_ * pictureHeight_);
    for (size_t i = 0; i < total; ++i) {
        uint8_t v = lumaBuffer_[i];
        backRgbaBuffer_[i] = (0xFF000000) | (static_cast<uint32_t>(v) << 16) | (static_cast<uint32_t>(v) << 8) | v;
    }
    std::swap(frontRgbaBuffer_, backRgbaBuffer_);
    newFrameAvailable_ = true;
    framesDecoded_++;
    syncLocked_ = (syncHits_ > 20);
    if (!syncLocked_) {
        consecutiveHsync_ = 0;
    }
    syncHits_ = 0;
}

void AnalogTvDemod::drawPixel(int mag) {
    // 1. Sync Slicing Filter with sub-sample edge tracking
    float prevFilter = syncMagFilter_;
    syncMagFilterPrev_ = prevFilter;
    syncMagFilter_ = syncMagFilter_ * 0.70f + static_cast<float>(mag) * 0.30f;
    bool inSync = (syncMagFilter_ > static_cast<float>(detectLevel_));

    if (inSync) {
        syncPulseSamples_++;
    } else {
        if (syncPulseSamples_ > 0) {
            double sampleRate = sampleRateHz_;
            // Pulse duration thresholds:
            // PAL: H-sync = 4.7 us (~11 samples at 2.4 MSPS). Broad V-sync = 27.3 us (~65 samples).
            int vSyncThresh = static_cast<int>(sampleRate * 20.0e-6); // >= 20 us is VBI broad pulse
            int hSyncMin    = static_cast<int>(sampleRate * 3.0e-6);  // >= 3.0 us
            int hSyncMax    = static_cast<int>(sampleRate * 6.5e-6);  // <= 6.5 us

            if (syncPulseSamples_ >= vSyncThresh) {
                // Vertical sync detected!
                // Hold-off gate: only accept V-sync if at least minFieldLines have passed.
                // This completely prevents false V-sync triggers from black bars, dark scenes, or noise!
                int minFieldLines = (config_.standard == TvStandard::NTSC_M) ? 220 : 270;
                if (linesSinceVSync_ >= minFieldLines) {
                    isProgressive_ = (linesSinceVSync_ > 450);
                    linesSinceVSync_ = 0;
                    if (config_.autoSync) {
                        commitFrame();
                        // Detect whether this is Field 1 (starts at line boundary) or Field 2 (starts at half-line)
                        isField2_ = (linePhase_ > 0.25 && linePhase_ < 0.75);
                        fieldLine_ = 0;
                        vbiCountdown_ = (config_.standard == TvStandard::NTSC_M) ? 19 : 22;
                    }
                }
            } else if (syncPulseSamples_ >= hSyncMin && syncPulseSamples_ <= hSyncMax) {
                // Horizontal sync detected!
                syncHits_++;
                if (config_.autoSync) {
                    // Nominal falling edge of H-sync pulse is at 4.7 us / 64 us = 0.0734375
                    double expectedPhase = 0.0734375;

                    // Linear interpolation of sub-sample edge crossing
                    float frac = 0.5f;
                    if (std::abs(prevFilter - syncMagFilter_) > 0.001f) {
                        frac = (prevFilter - static_cast<float>(detectLevel_)) / (prevFilter - syncMagFilter_);
                        frac = std::clamp(frac, 0.0f, 1.0f);
                    }

                    double phaseStepNominal = lineFrequency_ / sampleRateHz_;
                    double measuredPhase = linePhase_ - (1.0 - static_cast<double>(frac)) * phaseStepNominal;

                    double phaseErr = measuredPhase - expectedPhase;
                    while (phaseErr > 0.5) phaseErr -= 1.0;
                    while (phaseErr < -0.5) phaseErr += 1.0;

                    if (!syncLocked_ || consecutiveHsync_ < 8) {
                        // Rapid acquisition: snap phase directly to acquire lock immediately regardless of starting phase!
                        linePhase_ = expectedPhase;
                        pllPhaseError_ = 0.0;
                        consecutiveHsync_++;
                    } else if (std::abs(phaseErr) < 0.20) {
                        consecutiveHsync_++;
                        // Flywheel tracking: smooth proportional adjustment (stable, zero wobble)
                        pllPhaseError_ = pllPhaseError_ * 0.90 + phaseErr * 0.10;
                        linePhase_ -= pllPhaseError_ * 0.04;
                        if (linePhase_ < 0.0) linePhase_ += 1.0;
                        if (linePhase_ >= 1.0) linePhase_ -= 1.0;
                    }
                }
            }
            syncPulseSamples_ = 0;
        }
    }

    // 2. Fractional Phase Stepping
    double phaseStep = lineFrequency_ / sampleRateHz_;
    phaseStep += (static_cast<double>(config_.fineFrequencyCorrection) * 1e-6);
    linePhase_ += phaseStep;

    // 3. Line Advance
    int activeFieldLines = (config_.standard == TvStandard::NTSC_M) ? 240 : 288;
    if (isProgressive_) {
        activeFieldLines = (config_.standard == TvStandard::NTSC_M) ? 480 : 576;
    }

    if (linePhase_ >= 1.0) {
        linePhase_ -= 1.0;
        prevEffX_ = 0;
        linesSinceVSync_++;

        if (vbiCountdown_ > 0) {
            vbiCountdown_--;
        } else {
            fieldLine_++;
            if (fieldLine_ >= activeFieldLines) {
                fieldLine_ = 0;
                commitFrame();
            }
        }
    }

    // 4. Active video drawing
    // In European CCIR PAL standard:
    // H-sync = 0 to 4.7 us (0.0 to 0.0734)
    // Back porch = 4.7 to 12.0 us (0.0734 to 0.1875)
    // Active video = 12.0 to 62.5 us (0.1875 to 0.9765625)
    const double activeStart = 0.1875;
    const double activeEnd = 0.9765625;

    if (vbiCountdown_ == 0 && fieldLine_ < activeFieldLines && linePhase_ >= activeStart && linePhase_ <= activeEnd) {
        double u = (linePhase_ - activeStart) / (activeEnd - activeStart);
        // Manual horizontal shift (-100 to +100 percent)
        if (!config_.autoSync) {
            u += (static_cast<double>(config_.manualX) / 100.0);
        }
        while (u < 0.0) u += 1.0;
        while (u >= 1.0) u -= 1.0;

        int effX = std::max(0, std::min(pictureWidth_ - 1, static_cast<int>(u * pictureWidth_)));

        float pixelVal = static_cast<float>(blackLevel_ - mag) * scaleCoeff_ * config_.contrast + static_cast<float>(config_.brightness);
        pixelVal = std::max(0.0f, std::min(255.0f, pixelVal));
        uint8_t lumaByte = static_cast<uint8_t>(pixelVal);

        if (isProgressive_) {
            int effY = fieldLine_ + config_.manualY;
            while (effY < 0) effY += pictureHeight_;
            while (effY >= pictureHeight_) effY -= pictureHeight_;

            int lineOffset = effY * pictureWidth_;
            if (prevEffX_ >= 0 && prevEffX_ < effX) {
                for (int x = prevEffX_; x <= effX; ++x) {
                    lumaBuffer_[lineOffset + x] = lumaByte;
                }
            } else {
                lumaBuffer_[lineOffset + effX] = lumaByte;
            }
        } else if (config_.deinterlace) {
            // Full-Height Deinterlace (Line Doubling):
            // Maps each 288-line field to display row pair 2k and 2k+1.
            int y0 = fieldLine_ * 2 + config_.manualY;
            while (y0 < 0) y0 += pictureHeight_;
            while (y0 >= pictureHeight_) y0 -= pictureHeight_;

            int y1 = (y0 + 1) % pictureHeight_;

            int lineOffset0 = y0 * pictureWidth_;
            int lineOffset1 = y1 * pictureWidth_;

            if (prevEffX_ >= 0 && prevEffX_ < effX) {
                for (int x = prevEffX_; x <= effX; ++x) {
                    lumaBuffer_[lineOffset0 + x] = lumaByte;
                    lumaBuffer_[lineOffset1 + x] = lumaByte;
                }
            } else {
                lumaBuffer_[lineOffset0 + effX] = lumaByte;
                lumaBuffer_[lineOffset1 + effX] = lumaByte;
            }
        } else {
            // Classic 2:1 Interlace Weave:
            // Field 1 writes even rows (2k), Field 2 writes odd rows (2k + 1).
            int effY = fieldLine_ * 2 + (isField2_ ? 1 : 0) + config_.manualY;
            while (effY < 0) effY += pictureHeight_;
            while (effY >= pictureHeight_) effY -= pictureHeight_;

            int lineOffset = effY * pictureWidth_;
            if (prevEffX_ >= 0 && prevEffX_ < effX) {
                for (int x = prevEffX_; x <= effX; ++x) {
                    lumaBuffer_[lineOffset + x] = lumaByte;
                }
            } else {
                lumaBuffer_[lineOffset + effX] = lumaByte;
            }
        }
        prevEffX_ = effX + 1;
    } else if (linePhase_ > activeEnd) {
        prevEffX_ = -1;
    }

    // 5. Record waveform for oscilloscope
    int waveX = static_cast<int>(linePhase_ * latestScanline_.size());
    if (waveX >= 0 && waveX < static_cast<int>(latestScanline_.size())) {
        latestScanline_[waveX] = static_cast<float>(mag);
    }
}

void AnalogTvDemod::clearFrame() {
    std::lock_guard<std::mutex> lock(frameMutex_);
    std::fill(lumaBuffer_.begin(), lumaBuffer_.end(), static_cast<uint8_t>(16));
    std::fill(frontRgbaBuffer_.begin(), frontRgbaBuffer_.end(), 0xFF000000);
    std::fill(backRgbaBuffer_.begin(), backRgbaBuffer_.end(), 0xFF000000);
    consecutiveHsync_ = 0;
}

void AnalogTvDemod::processAudio(float iVal, float qVal) {
    if (!config_.audioEnabled) return;

    // Shift by sound subcarrier frequency
    double phaseInc = 2.0 * M_PI * soundSubcarrierHz_ / sampleRateHz_;
    soundPhase_ += phaseInc;
    if (soundPhase_ > 2.0 * M_PI) soundPhase_ -= 2.0 * M_PI;

    float cosS = static_cast<float>(std::cos(soundPhase_));
    float sinS = static_cast<float>(std::sin(soundPhase_));

    // Complex heterodyne mix
    float audioI = iVal * cosS + qVal * sinS;
    float audioQ = -iVal * sinS + qVal * cosS;

    // FM Demodulator: angle difference
    float curAngle = std::atan2(audioQ, audioI);
    float deltaAngle = curAngle - prevAudioAngle_;
    prevAudioAngle_ = curAngle;

    // Unwrap angle
    if (deltaAngle > static_cast<float>(M_PI))  deltaAngle -= static_cast<float>(2.0 * M_PI);
    if (deltaAngle < static_cast<float>(-M_PI)) deltaAngle += static_cast<float>(2.0 * M_PI);

    // Single-pole 50 us low-pass de-emphasis filter
    float alpha = static_cast<float>(1.0 / (1.0 + (50e-6 * sampleRateHz_)));
    deemphState_ = deemphState_ + alpha * (deltaAngle - deemphState_);

    // Decimate to 48 kHz
    static int audioDecimator = 0;
    int decimRatio = std::max(1, static_cast<int>(sampleRateHz_ / 48000.0));
    if (++audioDecimator >= decimRatio) {
        audioDecimator = 0;
        float pcm = deemphState_ * config_.audioVolume * 2.5f;
        pcm = std::max(-1.0f, std::min(1.0f, pcm));

        std::lock_guard<std::mutex> lock(audioMutex_);
        audioRingBuffer_[audioWriteIdx_] = pcm;
        audioWriteIdx_ = (audioWriteIdx_ + 1) % audioRingBuffer_.size();
    }
}

void AnalogTvDemod::processIqBytes(const uint8_t* iqData, size_t length) {
    if (!iqData || length < 2) return;

    size_t numPairs = length / 2;
    int maxMagInBatch = 0;

    for (size_t k = 0; k < numPairs; ++k) {
        float iNorm = static_cast<float>(iqData[k * 2 + 0]) - 127.5f;
        float qNorm = static_cast<float>(iqData[k * 2 + 1]) - 127.5f;

        // Envelope detection: M = sqrt(I^2 + Q^2)
        int mag = static_cast<int>(std::sqrt(iNorm * iNorm + qNorm * qNorm));
        maxMagInBatch = std::max(maxMagInBatch, mag);

        if (config_.inverseVideo) {
            mag = static_cast<int>(maxSignalLevel_) - mag;
        }

        drawPixel(mag);
        processAudio(iNorm / 128.0f, qNorm / 128.0f);
    }

    // Dynamic Automatic Gain Control (AGC) tracking
    maxSignalLevel_ = maxSignalLevel_ * 0.90f + static_cast<float>(maxMagInBatch) * 0.10f;
    detectLevel_ = static_cast<int>(maxSignalLevel_ * config_.detectorLevelCoef);
    blackLevel_ = static_cast<int>(maxSignalLevel_ * 0.70f);
    scaleCoeff_ = 255.0f / std::max(1.0f, static_cast<float>(blackLevel_ * 0.85f));
}

bool AnalogTvDemod::getFrame(std::vector<uint32_t>& outRgba, int& outWidth, int& outHeight) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    outWidth = pictureWidth_;
    outHeight = pictureHeight_;
    if (frontRgbaBuffer_.empty()) return false;
    outRgba = frontRgbaBuffer_;
    return true;
}

void AnalogTvDemod::getWaveform(std::vector<float>& outWaveform, float& outSyncLevel, float& outBlackLevel) {
    std::lock_guard<std::mutex> lock(frameMutex_);
    outWaveform = latestScanline_;
    outSyncLevel = static_cast<float>(detectLevel_);
    outBlackLevel = static_cast<float>(blackLevel_);
}

size_t AnalogTvDemod::getAudioSamples(float* dest, size_t maxSamples) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    size_t count = 0;
    while (count < maxSamples && audioReadIdx_ != audioWriteIdx_) {
        dest[count++] = audioRingBuffer_[audioReadIdx_];
        audioReadIdx_ = (audioReadIdx_ + 1) % audioRingBuffer_.size();
    }
    return count;
}

TvStats AnalogTvDemod::getStats() const {
    std::lock_guard<std::mutex> lock(frameMutex_);
    TvStats st;
    st.hasSyncLock = syncLocked_;
    st.lineFrequencyHz = static_cast<float>(lineFrequency_);
    st.frameRateFps = (config_.standard == TvStandard::NTSC_M) ? 29.97f : 25.0f;
    st.detectedLines = pictureHeight_;
    st.pictureWidth = pictureWidth_;
    st.pictureHeight = pictureHeight_;
    st.syncSignalLevel = static_cast<float>(detectLevel_);
    st.blackLevel = static_cast<float>(blackLevel_);
    st.framesDecoded = framesDecoded_;
    return st;
}

} // namespace RTLSDR
