#include "RtlUsbDevice.hpp"
#include <iostream>
#include <cstring>

#ifdef HAVE_LIBRTLSDR
#include <rtl-sdr.h>
#endif

namespace RTLSDR {

RtlUsbDevice::RtlUsbDevice() = default;

RtlUsbDevice::~RtlUsbDevice() {
    close();
}

void RtlUsbDevice::rtlsdrAsyncCallback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* self = static_cast<RtlUsbDevice*>(ctx);
    if (!self || !self->isRunning_) return;

    if (self->callback_) {
        self->callback_(buf, len);
    }

    std::lock_guard<std::mutex> lock(self->statsMutex_);
    self->stats_.bytesReceived += len;
}

bool RtlUsbDevice::open(const SdrConfig& config) {
    close();
    config_ = config;

#ifdef HAVE_LIBRTLSDR
    uint32_t deviceCount = rtlsdr_get_device_count();
    if (deviceCount == 0) {
        std::cerr << "[RtlUsbDevice] No RTL-SDR USB devices found on host.\n";
        return false;
    }

    char vendor[256], product[256], serial[256];
    rtlsdr_get_device_usb_strings(0, vendor, product, serial);
    std::cout << "[RtlUsbDevice] Found: " << vendor << " " << product << " (SN: " << serial << ")\n";

    if (rtlsdr_open(&dev_, 0) < 0) {
        std::cerr << "[RtlUsbDevice] Failed to open RTL-SDR device\n";
        return false;
    }

    rtlsdr_set_sample_rate(dev_, config.sampleRateHz);
    rtlsdr_set_center_freq(dev_, config.frequencyHz);

    if (config.tunerAgc) {
        rtlsdr_set_tuner_gain_mode(dev_, 0); // Automatic
    } else {
        rtlsdr_set_tuner_gain_mode(dev_, 1); // Manual
        rtlsdr_set_tuner_gain(dev_, config.tunerGainTenthsDb);
    }

    rtlsdr_set_agc_mode(dev_, config.rtlAgc ? 1 : 0);
    rtlsdr_set_freq_correction(dev_, config.frequencyCorrectionPpm);
    rtlsdr_set_direct_sampling(dev_, config.directSampling);
    rtlsdr_reset_buffer(dev_);

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.connected = true;
        stats_.deviceName = std::string(product) + " (" + serial + ")";
        stats_.tunerType = "Local USB R820T/RTL2832U";
        stats_.bytesReceived = 0;
        stats_.currentKBytesPerSec = static_cast<float>(config.sampleRateHz * 2) / 1024.0f;
    }

    shouldStop_ = false;
    isRunning_ = true;
    workerThread_ = std::thread(&RtlUsbDevice::asyncReadLoop, this);
    return true;

#else
    std::cerr << "[RtlUsbDevice] Built without librtlsdr support. Please connect via rtl_tcp to Pi 4.\n";
    return false;
#endif
}

void RtlUsbDevice::close() {
    shouldStop_ = true;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_cancel_async(dev_);
    }
#endif
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_close(dev_);
        dev_ = nullptr;
    }
#endif
    isRunning_ = false;

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.connected = false;
    stats_.currentKBytesPerSec = 0.0f;
}

void RtlUsbDevice::setFrequency(uint32_t frequencyHz) {
    config_.frequencyHz = frequencyHz;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_center_freq(dev_, frequencyHz);
    }
#endif
}

void RtlUsbDevice::setSampleRate(uint32_t sampleRateHz) {
    config_.sampleRateHz = sampleRateHz;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_sample_rate(dev_, sampleRateHz);
    }
#endif
}

void RtlUsbDevice::setGain(int gainTenthsDb) {
    config_.tunerGainTenthsDb = gainTenthsDb;
    config_.tunerAgc = false;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_tuner_gain_mode(dev_, 1);
        rtlsdr_set_tuner_gain(dev_, gainTenthsDb);
    }
#endif
}

void RtlUsbDevice::setTunerAgc(bool enable) {
    config_.tunerAgc = enable;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_tuner_gain_mode(dev_, enable ? 0 : 1);
    }
#endif
}

void RtlUsbDevice::setRtlAgc(bool enable) {
    config_.rtlAgc = enable;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_agc_mode(dev_, enable ? 1 : 0);
    }
#endif
}

void RtlUsbDevice::setFrequencyCorrection(int ppm) {
    config_.frequencyCorrectionPpm = ppm;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_freq_correction(dev_, ppm);
    }
#endif
}

void RtlUsbDevice::setDirectSampling(int mode) {
    config_.directSampling = mode;
#ifdef HAVE_LIBRTLSDR
    if (dev_) {
        rtlsdr_set_direct_sampling(dev_, mode);
    }
#endif
}

SdrStats RtlUsbDevice::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void RtlUsbDevice::asyncReadLoop() {
#ifdef HAVE_LIBRTLSDR
    if (!dev_) return;
    // Read async: 15 buffers, 16384 bytes each
    rtlsdr_read_async(dev_, rtlsdrAsyncCallback, this, 15, 16384);
#endif
    isRunning_ = false;
}

} // namespace RTLSDR
