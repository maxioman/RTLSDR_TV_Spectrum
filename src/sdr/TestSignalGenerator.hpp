#pragma once

#include "ISdrDevice.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace RTLSDR {

enum class TestPatternType {
    PM5544_TESTCARD = 0,
    EBU_COLORBARS   = 1,
    CUSTOM_IMAGE    = 2
};

class TestSignalGenerator : public ISdrDevice {
public:
    TestSignalGenerator();
    ~TestSignalGenerator() override;

    bool open(const SdrConfig& config) override;
    void close() override;
    bool isRunning() const override { return isRunning_; }

    void setFrequency(uint32_t frequencyHz) override;
    void setSampleRate(uint32_t sampleRateHz) override;
    void setGain(int gainTenthsDb) override;
    void setTunerAgc(bool enable) override;
    void setRtlAgc(bool enable) override;
    void setFrequencyCorrection(int ppm) override;
    void setDirectSampling(int mode) override;

    void setCallback(SamplesCallback cb) override { callback_ = cb; }
    SdrStats getStats() const override;

    // Pattern & Image transmission controls
    void setPatternType(TestPatternType type);
    TestPatternType getPatternType() const;

    bool loadCustomImage(const std::string& filePath);
    std::string getLoadedImageInfo() const;

    void setNoiseLevel(float noisePct);
    float getNoiseLevel() const;

private:
    void generatorThreadLoop();
    void initPM5544Testcard();

    std::atomic<bool> isRunning_{false};
    std::atomic<bool> shouldStop_{false};
    std::thread workerThread_;

    SdrConfig config_;
    mutable std::mutex statsMutex_;
    SdrStats stats_;

    SamplesCallback callback_;
    uint64_t sampleIndex_ = 0;

    std::atomic<TestPatternType> patternType_{TestPatternType::PM5544_TESTCARD};
    std::atomic<float> noiseLevel_{0.04f}; // 4% default studio noise

    // Image buffers (grayscale luma 0-255)
    std::vector<uint8_t> pm5544Image_;
    int pm5544Width_ = 768;
    int pm5544Height_ = 576;

    std::mutex customImageMutex_;
    std::vector<uint8_t> customImage_;
    int customWidth_ = 0;
    int customHeight_ = 0;
    std::string customImageInfo_ = "No custom image loaded";
};

} // namespace RTLSDR
