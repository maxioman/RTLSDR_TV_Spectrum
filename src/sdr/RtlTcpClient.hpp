#pragma once

#include "ISdrDevice.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>

namespace RTLSDR {

class RtlTcpClient : public ISdrDevice {
public:
    RtlTcpClient();
    ~RtlTcpClient() override;

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

private:
    void networkThreadLoop();
    bool sendCommand(uint8_t cmd, uint32_t param);

    int sockfd_ = -1;
    std::atomic<bool> isRunning_{false};
    std::atomic<bool> shouldStop_{false};
    std::thread workerThread_;

    SdrConfig config_;
    mutable std::mutex statsMutex_;
    SdrStats stats_;

    SamplesCallback callback_;
};

} // namespace RTLSDR
