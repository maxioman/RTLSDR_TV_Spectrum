#include "TestSignalGenerator.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <cmath>
#include <chrono>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace RTLSDR {

TestSignalGenerator::TestSignalGenerator() {
    initPM5544Testcard();
}

TestSignalGenerator::~TestSignalGenerator() {
    close();
}

bool TestSignalGenerator::open(const SdrConfig& config) {
    close();
    config_ = config;

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.connected = true;
        stats_.deviceName = "Synthetic PAL AM-VSB RF & Broadcast Testcard Generator";
        stats_.tunerType = "Simulated RTL2832U / R820T";
        stats_.bytesReceived = 0;
        stats_.currentKBytesPerSec = static_cast<float>(config.sampleRateHz * 2) / 1024.0f;
    }

    shouldStop_ = false;
    isRunning_ = true;
    workerThread_ = std::thread(&TestSignalGenerator::generatorThreadLoop, this);
    return true;
}

void TestSignalGenerator::close() {
    shouldStop_ = true;
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    isRunning_ = false;

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.connected = false;
    stats_.currentKBytesPerSec = 0.0f;
}

void TestSignalGenerator::setFrequency(uint32_t frequencyHz) {
    config_.frequencyHz = frequencyHz;
}

void TestSignalGenerator::setSampleRate(uint32_t sampleRateHz) {
    config_.sampleRateHz = sampleRateHz;
}

void TestSignalGenerator::setGain(int gainTenthsDb) {
    config_.tunerGainTenthsDb = gainTenthsDb;
}

void TestSignalGenerator::setTunerAgc(bool enable) {
    config_.tunerAgc = enable;
}

void TestSignalGenerator::setRtlAgc(bool enable) {
    config_.rtlAgc = enable;
}

void TestSignalGenerator::setFrequencyCorrection(int ppm) {
    config_.frequencyCorrectionPpm = ppm;
}

void TestSignalGenerator::setDirectSampling(int mode) {
    config_.directSampling = mode;
}

SdrStats TestSignalGenerator::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void TestSignalGenerator::setPatternType(TestPatternType type) {
    patternType_ = type;
}

TestPatternType TestSignalGenerator::getPatternType() const {
    return patternType_.load();
}

bool TestSignalGenerator::loadCustomImage(const std::string& filePath) {
    int w = 0, h = 0, comp = 0;
    unsigned char* data = stbi_load(filePath.c_str(), &w, &h, &comp, 1);
    if (!data) {
        std::lock_guard<std::mutex> lock(customImageMutex_);
        customImageInfo_ = "Error loading: " + filePath;
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(customImageMutex_);
        customWidth_ = w;
        customHeight_ = h;
        customImage_.assign(data, data + (w * h));
        customImageInfo_ = "Loaded (" + std::to_string(w) + "x" + std::to_string(h) + ")";
        patternType_ = TestPatternType::CUSTOM_IMAGE;
    }

    stbi_image_free(data);
    return true;
}

std::string TestSignalGenerator::getLoadedImageInfo() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(customImageMutex_));
    return customImageInfo_;
}

void TestSignalGenerator::setNoiseLevel(float noisePct) {
    noiseLevel_ = std::max(0.0f, std::min(1.0f, noisePct));
}

float TestSignalGenerator::getNoiseLevel() const {
    return noiseLevel_.load();
}

void TestSignalGenerator::initPM5544Testcard() {
    pm5544Width_ = 768;
    pm5544Height_ = 576;
    pm5544Image_.assign(pm5544Width_ * pm5544Height_, 128); // 50% grey background

    int cx = pm5544Width_ / 2;
    int cy = pm5544Height_ / 2;
    int radius = 240;

    // 1. Crosshatch grid (white lines every 48 pixels)
    for (int y = 0; y < pm5544Height_; ++y) {
        for (int x = 0; x < pm5544Width_; ++x) {
            if ((x % 48 == 0 || x % 48 == 1) || (y % 48 == 0 || y % 48 == 1)) {
                pm5544Image_[y * pm5544Width_ + x] = 230;
            }
        }
    }

    // 2. Outer border castellation blocks (16px border)
    for (int y = 0; y < pm5544Height_; ++y) {
        for (int x = 0; x < pm5544Width_; ++x) {
            bool isBorder = (x < 16 || x >= pm5544Width_ - 16 || y < 16 || y >= pm5544Height_ - 16);
            if (isBorder) {
                int block = (x / 24) + (y / 24);
                pm5544Image_[y * pm5544Width_ + x] = (block % 2 == 0) ? 235 : 16;
            }
        }
    }

    // 3. Circle Interior
    for (int y = 0; y < pm5544Height_; ++y) {
        int dy = y - cy;
        for (int x = 0; x < pm5544Width_; ++x) {
            int dx = x - cx;
            int distSq = dx * dx + dy * dy;

            // Border ring of circle
            if (distSq >= (radius - 3) * (radius - 3) && distSq <= radius * radius) {
                pm5544Image_[y * pm5544Width_ + x] = 235; // White ring
                continue;
            }

            if (distSq < (radius - 3) * (radius - 3)) {
                uint8_t pixel = 128;

                if (dy < -160) {
                    // Top checkerboard segment
                    int bx = (x - cx + 240) / 24;
                    int by = (dy + 240) / 20;
                    pixel = ((bx + by) % 2 == 0) ? 235 : 16;
                } else if (dy < -40) {
                    // Color bars: 8 PAL bars across x from cx - 192 to cx + 192
                    int barW = 48;
                    int barIdx = (x - (cx - 192)) / barW;
                    if (barIdx >= 0 && barIdx < 8) {
                        const uint8_t palLuma[8] = { 235, 210, 170, 145, 106, 81, 41, 16 };
                        pixel = palLuma[barIdx];
                    } else {
                        pixel = 128;
                    }
                } else if (dy < 24) {
                    // Station Identification Box
                    if (std::abs(dx) < 140 && dy > -32 && dy < 16) {
                        pixel = 16; // Black box
                        if (std::abs(dx) > 136 || dy < -29 || dy > 13) {
                            pixel = 235; // White box border
                        }
                    } else {
                        pixel = 128;
                    }
                } else if (dy < 110) {
                    // Multiburst resolution wedges (5 frequency gratings)
                    int blockW = 60;
                    int blockIdx = (x - (cx - 150)) / blockW;
                    if (blockIdx >= 0 && blockIdx < 5) {
                        const int periods[5] = { 16, 10, 6, 4, 2 };
                        int p = periods[blockIdx];
                        pixel = ((x / (p / 2)) % 2 == 0) ? 235 : 16;
                    } else {
                        pixel = 128;
                    }
                } else if (dy < 175) {
                    // Grayscale staircase (6 steps from 0% to 100%)
                    int stepW = 50;
                    int stepIdx = (x - (cx - 150)) / stepW;
                    if (stepIdx >= 0 && stepIdx < 6) {
                        const uint8_t steps[6] = { 16, 60, 104, 148, 192, 235 };
                        pixel = steps[stepIdx];
                    } else {
                        pixel = 128;
                    }
                } else {
                    // Bottom segment
                    int bx = (x - cx + 240) / 24;
                    int by = (dy + 240) / 20;
                    pixel = ((bx + by) % 2 == 0) ? 235 : 16;
                }

                pm5544Image_[y * pm5544Width_ + x] = pixel;
            }
        }
    }

    // 4. Render "RTL-SDR TV" text in station ID box
    auto drawChar5x7 = [this](char c, int px, int py, int scale, uint8_t color) {
        static const uint8_t FONT_R[7] = { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 };
        static const uint8_t FONT_T[7] = { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 };
        static const uint8_t FONT_L[7] = { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F };
        static const uint8_t FONT_MINUS[7] = { 0x00, 0x00, 0x00, 0x1F, 0x00, 0x00, 0x00 };
        static const uint8_t FONT_S[7] = { 0x0E, 0x11, 0x10, 0x0E, 0x01, 0x11, 0x0E };
        static const uint8_t FONT_D[7] = { 0x1C, 0x12, 0x11, 0x11, 0x11, 0x12, 0x1C };
        static const uint8_t FONT_V[7] = { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 };

        const uint8_t* glyph = nullptr;
        if (c == 'R') glyph = FONT_R;
        else if (c == 'T') glyph = FONT_T;
        else if (c == 'L') glyph = FONT_L;
        else if (c == '-') glyph = FONT_MINUS;
        else if (c == 'S') glyph = FONT_S;
        else if (c == 'D') glyph = FONT_D;
        else if (c == 'V') glyph = FONT_V;

        if (!glyph) return;

        for (int row = 0; row < 7; ++row) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 5; ++col) {
                if (bits & (0x10 >> col)) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            int gx = px + col * scale + sx;
                            int gy = py + row * scale + sy;
                            if (gx >= 0 && gx < pm5544Width_ && gy >= 0 && gy < pm5544Height_) {
                                pm5544Image_[gy * pm5544Width_ + gx] = color;
                            }
                        }
                    }
                }
            }
        }
    };

    const char* label = "RTL-SDR TV";
    int scale = 3;
    int charStep = 6 * scale;
    int totalTextW = 10 * charStep;
    int textX = cx - totalTextW / 2 + 6;
    int textY = cy - 18;
    for (int i = 0; label[i] != '\0'; ++i) {
        drawChar5x7(label[i], textX + i * charStep, textY, scale, 235);
    }
}

void TestSignalGenerator::generatorThreadLoop() {
    constexpr size_t BLOCK_SAMPLES = 16384; // 16k complex samples = 32 KB I/Q
    std::vector<uint8_t> iqBuffer(BLOCK_SAMPLES * 2);

    const double lineRate = 15625.0; // PAL 625/50 line rate (15.625 kHz)

    auto nextTick = std::chrono::steady_clock::now();

    while (!shouldStop_) {
        double sRate = static_cast<double>(config_.sampleRateHz);
        double samplesPerLine = sRate / lineRate;

        for (size_t i = 0; i < BLOCK_SAMPLES; ++i) {
            double sampleTime = static_cast<double>(sampleIndex_) / sRate;
            double linePos = std::fmod(static_cast<double>(sampleIndex_), samplesPerLine) / samplesPerLine;
            double lineInFrameDouble = std::fmod(static_cast<double>(sampleIndex_) / samplesPerLine, 625.0);
            int lineInFrame = static_cast<int>(lineInFrameDouble);

            // Base envelope level: Negative modulation TV signal
            // Max carrier envelope (1.0) = Sync tip (-40 IRE)
            // Black level = 0.70 (0 IRE)
            // White level = 0.10 (100 IRE)
            float envelope = 0.70f; // Default black level

            // Standard PAL CCIR 625/50 interlaced structure (Field 1: lines 0..311, Field 2: lines 312..624)
            bool isField2 = (lineInFrame >= 312);
            bool inVbi = false;
            bool isSerrationLine = false;
            int scanlineY = 0;

            if (!isField2) {
                // Field 1: lines 0..311
                if (lineInFrame < 23 || lineInFrame >= 311) {
                    inVbi = true;
                    isSerrationLine = (lineInFrame >= 2 && lineInFrame <= 4);
                } else {
                    scanlineY = (lineInFrame - 23) * 2; // Even rows: 0, 2, 4, ..., 574
                }
            } else {
                // Field 2: lines 312..624
                if (lineInFrame < 336 || lineInFrame >= 624) {
                    inVbi = true;
                    isSerrationLine = (lineInFrame >= 314 && lineInFrame <= 316);
                } else {
                    scanlineY = (lineInFrame - 336) * 2 + 1; // Odd rows: 1, 3, 5, ..., 575
                }
            }

            if (inVbi) {
                if (isSerrationLine) {
                    if (!isField2) {
                        // Field 1 serration: broad pulse (~27 us sync pulse) aligned with line start
                        envelope = (linePos < 0.85) ? 1.0f : 0.70f;
                    } else {
                        // Field 2 serration: broad pulse offset by half line (32 us)
                        double f2Pos = std::fmod(linePos + 0.5, 1.0);
                        envelope = (f2Pos < 0.85) ? 1.0f : 0.70f;
                    }
                } else {
                    // Standard lines during VBI (normal H-sync)
                    envelope = (linePos < 0.0734375) ? 1.0f : 0.70f;
                }
            } else {
                // Active video line
                if (linePos < 0.0734375) {
                    // Horizontal sync pulse (4.7 us sync tip = peak carrier)
                    envelope = 1.00f;
                } else if (linePos < 0.1875) {
                    // Back porch (black level + 4.43 MHz PAL color burst)
                    float burst = 0.0f;
                    if (linePos > 0.09 && linePos < 0.14) {
                        burst = std::sin(2.0 * M_PI * 4.433619e6 * sampleTime) * 0.10f;
                    }
                    envelope = 0.70f + burst;
                } else if (linePos < 0.9765625) {
                    // Active video region (50.5 us)
                    double u = (linePos - 0.1875) / (0.9765625 - 0.1875);

                    // Scanline index y in [0, 575] across interlaced fields
                    int y = std::max(0, std::min(575, scanlineY));

                    uint8_t lumaByte = 128;
                    TestPatternType pType = patternType_.load();

                    if (pType == TestPatternType::CUSTOM_IMAGE) {
                        std::lock_guard<std::mutex> lock(customImageMutex_);
                        if (!customImage_.empty() && customWidth_ > 0 && customHeight_ > 0) {
                            int sx = std::max(0, std::min(customWidth_ - 1, static_cast<int>(u * customWidth_)));
                            int sy = std::max(0, std::min(customHeight_ - 1, static_cast<int>(static_cast<float>(y) / 576.0f * customHeight_)));
                            lumaByte = customImage_[sy * customWidth_ + sx];
                        } else {
                            pType = TestPatternType::PM5544_TESTCARD;
                        }
                    }

                    if (pType == TestPatternType::EBU_COLORBARS) {
                        int barIndex = std::min(7, static_cast<int>(u * 8.0));
                        const uint8_t barLuma[8] = { 235, 210, 170, 145, 106, 81, 41, 16 };
                        lumaByte = barLuma[barIndex];
                    } else if (pType == TestPatternType::PM5544_TESTCARD) {
                        if (!pm5544Image_.empty()) {
                            int sx = std::max(0, std::min(pm5544Width_ - 1, static_cast<int>(u * pm5544Width_)));
                            lumaByte = pm5544Image_[y * pm5544Width_ + sx];
                        }
                    }

                    float lumaNorm = static_cast<float>(lumaByte) / 255.0f;
                    envelope = 0.70f - (lumaNorm * 0.60f); // 1.0 luma = 0.10 white, 0.0 luma = 0.70 black
                } else {
                    // Front porch (black level)
                    envelope = 0.70f;
                }
            }

            // Sound subcarrier at +5.5 MHz (PAL B/G sound carrier with 1 kHz audio tone)
            float soundMod = std::sin(2.0 * M_PI * 1000.0 * sampleTime) * 30000.0f; // 30 kHz dev
            float soundPhase = 2.0f * static_cast<float>(M_PI) * (5.5e6f + soundMod) * static_cast<float>(sampleTime);
            float soundCarrier = std::sin(soundPhase) * 0.10f;

            // Composite baseband RF envelope
            float rfTotal = envelope + soundCarrier;

            // Add RF channel noise
            float curNoiseLvl = noiseLevel_.load();
            float noise = ((static_cast<float>(std::rand() % 2000) / 1000.0f) - 1.0f) * curNoiseLvl;
            rfTotal += noise;

            // Generate I/Q baseband samples
            int sampleI = static_cast<int>(127.5f + (rfTotal * 90.0f));
            int sampleQ = static_cast<int>(127.5f + (noise * 80.0f));

            sampleI = std::max(0, std::min(255, sampleI));
            sampleQ = std::max(0, std::min(255, sampleQ));

            iqBuffer[i * 2 + 0] = static_cast<uint8_t>(sampleI);
            iqBuffer[i * 2 + 1] = static_cast<uint8_t>(sampleQ);

            sampleIndex_++;
        }

        if (callback_) {
            callback_(iqBuffer.data(), iqBuffer.size());
        }

        {
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.bytesReceived += iqBuffer.size();
        }

        // Pace execution to match real-time sample rate
        double blockDurationSec = static_cast<double>(BLOCK_SAMPLES) / sRate;
        nextTick += std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(blockDurationSec)
        );
        std::this_thread::sleep_until(nextTick);
    }
}

} // namespace RTLSDR
