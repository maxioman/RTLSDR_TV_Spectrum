#pragma once

#include <vector>
#include <cstdint>
#include <mutex>
#include <string>

namespace RTLSDR {

enum class TvStandard {
    PAL_B_G = 0, // 625 lines / 50 Hz, 15625 Hz, +5.5 MHz Sound (Negative AM)
    PAL_I   = 1, // 625 lines / 50 Hz, 15625 Hz, +6.0 MHz Sound (Negative AM)
    PAL_D_K = 2, // 625 lines / 50 Hz, 15625 Hz, +6.5 MHz Sound (Negative AM)
    NTSC_M  = 3, // 525 lines / 60 Hz, 15734.26 Hz, +4.5 MHz Sound (Negative AM)
    SECAM_L = 4  // 625 lines / 50 Hz, 15625 Hz, +6.5 MHz Sound (Positive AM)
};

struct TvConfig {
    TvStandard standard = TvStandard::PAL_B_G;
    int brightness = 0;              // -100 to 100
    float contrast = 1.0f;           // 0.2 to 3.0
    bool inverseVideo = false;       // True for positive modulation (System L), False for negative (PAL B/G)
    bool autoSync = true;            // Auto horizontal & vertical sync lock
    int manualX = 0;                 // Manual horizontal phase
    int manualY = 0;                 // Manual vertical phase
    int fineFrequencyCorrection = 0; // -100 to 100 fine line clock trimmer
    float detectorLevelCoef = 0.84f; // Slicing threshold (halfway between 70% black and 100% sync tip)
    bool deinterlace = true;         // Progressive deinterlaced raster (eliminates comb & step-ladder)
    bool audioEnabled = false;
    float audioVolume = 0.5f;
};

struct TvStats {
    bool hasSyncLock = false;
    float lineFrequencyHz = 15625.0f;
    float frameRateFps = 25.0f;
    int detectedLines = 625;
    int pictureWidth = 154;
    int pictureHeight = 625;
    float syncSignalLevel = 0.0f;
    float blackLevel = 0.0f;
    uint32_t framesDecoded = 0;
};

class AnalogTvDemod {
public:
    AnalogTvDemod();
    ~AnalogTvDemod() = default;

    void setConfig(const TvConfig& cfg);
    TvConfig getConfig() const;

    void setSampleRate(double sampleRateHz);

    // Process streaming unsigned 8-bit I/Q samples from SDR
    void processIqBytes(const uint8_t* iqData, size_t length);

    // Video display frame accessor (RGBA32 pixel buffer)
    bool getFrame(std::vector<uint32_t>& outRgba, int& outWidth, int& outHeight);

    // Latest active scanline waveform (for oscilloscope IRE scope)
    void getWaveform(std::vector<float>& outWaveform, float& outSyncLevel, float& outBlackLevel);

    // Decimated audio PCM samples (mono 48 kHz float: -1.0 to 1.0)
    size_t getAudioSamples(float* dest, size_t maxSamples);

    TvStats getStats() const;

    // Clear frame buffers (eliminates persistence when switching channels)
    void clearFrame();

private:
    void reconfigure();
    void commitFrame();
    void drawPixel(int magnitude);
    void fineFrequencyTick();
    void processAudio(float iVal, float qVal);

    TvConfig config_;
    double sampleRateHz_ = 2400000.0; // 2.4 MSPS

    int pictureWidth_ = 720; // Broadcast D1 width (crisp 720x576 / 720x480 active resolution)
    int pictureHeight_ = 576;
    int lineInFrame_ = 625;
    double lineFrequency_ = 15625.0;
    double soundSubcarrierHz_ = 5.5e6;

    // Flywheel PLL & Raster state
    double linePhase_ = 0.0;
    double pllPhaseError_ = 0.0;
    double pllFreqCorr_ = 0.0;
    int fieldLine_ = 0;
    bool isField2_ = false;
    int syncPulseSamples_ = 0;
    int fieldIndex_ = 0;
    int prevEffX_ = 0;
    int linesSinceVSync_ = 0;
    int vbiCountdown_ = 0;
    float syncMagFilter_ = 0.0f;
    float syncMagFilterPrev_ = 0.0f;
    bool isProgressive_ = false;
    int consecutiveHsync_ = 0;

    float maxSignalLevel_ = 120.0f;
    int detectLevel_ = 90;
    int blackLevel_ = 80;
    float scaleCoeff_ = 1.0f;

    // Frame buffers
    std::vector<uint8_t> lumaBuffer_;
    std::vector<uint32_t> frontRgbaBuffer_;
    std::vector<uint32_t> backRgbaBuffer_;
    std::vector<float> latestScanline_;

    // Audio demodulator state
    double soundPhase_ = 0.0;
    float prevAudioAngle_ = 0.0f;
    float deemphState_ = 0.0f;
    std::vector<float> audioRingBuffer_;
    size_t audioWriteIdx_ = 0;
    size_t audioReadIdx_ = 0;

    mutable std::mutex frameMutex_;
    mutable std::mutex audioMutex_;
    bool newFrameAvailable_ = false;
    uint32_t framesDecoded_ = 0;
    uint32_t syncHits_ = 0;
    bool syncLocked_ = false;
};

} // namespace RTLSDR
