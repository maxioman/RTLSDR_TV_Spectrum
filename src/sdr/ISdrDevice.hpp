#pragma once

#include "SdrTypes.hpp"
#include <cstdint>
#include <cstddef>
#include <functional>

namespace RTLSDR {

using SamplesCallback = std::function<void(const uint8_t* data, size_t length)>;

class ISdrDevice {
public:
    virtual ~ISdrDevice() = default;

    virtual bool open(const SdrConfig& config) = 0;
    virtual void close() = 0;
    virtual bool isRunning() const = 0;

    virtual void setFrequency(uint32_t frequencyHz) = 0;
    virtual void setSampleRate(uint32_t sampleRateHz) = 0;
    virtual void setGain(int gainTenthsDb) = 0;
    virtual void setTunerAgc(bool enable) = 0;
    virtual void setRtlAgc(bool enable) = 0;
    virtual void setFrequencyCorrection(int ppm) = 0;
    virtual void setDirectSampling(int mode) = 0;

    virtual void setCallback(SamplesCallback cb) = 0;
    virtual SdrStats getStats() const = 0;
};

} // namespace RTLSDR
