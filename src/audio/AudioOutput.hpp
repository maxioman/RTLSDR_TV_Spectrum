#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace RTLSDR {

class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    bool open(uint32_t sampleRate = 48000);
    void close();
    bool isOpen() const { return isOpen_; }

    // Push mono float PCM samples in [-1.0, 1.0]
    void pushSamples(const float* samples, size_t count);
    void clear();

private:
    size_t popSamples(float* dest, size_t maxCount);

    bool isOpen_ = false;
    uint32_t sampleRate_ = 48000;

    std::vector<float> ringBuffer_;
    size_t writeIdx_ = 0;
    size_t readIdx_ = 0;
    mutable std::mutex mutex_;

    struct Impl;
    Impl* impl_ = nullptr;
};

} // namespace RTLSDR
