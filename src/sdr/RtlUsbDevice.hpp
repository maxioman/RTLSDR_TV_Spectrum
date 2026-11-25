#pragma once

#include "ISdrDevice.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

struct rtlsdr_dev;

namespace RTLSDR {

class RtlUsbDevice : public ISdrDevice {
public:
    RtlUsbDevice();
    ~RtlUsbDevice() override;

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

    static void rtlsdrAsyncCallback(unsigned char* buf, uint32_t len, void* ctx);

private:
    void asyncReadLoop();

    rtlsdr_dev* dev_ = nullptr;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> shouldStop_{false};
    std::thread workerThread_;

    SdrConfig config_;
    mutable std::mutex statsMutex_;
    SdrStats stats_;

    SamplesCallback callback_;
};

} // namespace RTLSDR
