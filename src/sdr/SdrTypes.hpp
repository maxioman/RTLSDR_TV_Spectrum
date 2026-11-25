#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace RTLSDR {

enum class SdrSourceType {
    RtlTcpNetwork = 0,
    LocalRtlUsb   = 1,
    TestGenerator = 2
};

struct SdrConfig {
    SdrSourceType sourceType = SdrSourceType::RtlTcpNetwork;
    std::string remoteHost = "192.168.1.27";
    uint16_t remotePort = 1234;
    uint32_t frequencyHz = 175250000; // Default: VHF CH E5 PAL Video Carrier (175.25 MHz)
    uint32_t sampleRateHz = 2400000;  // 2.4 MSPS
    int tunerGainTenthsDb = 250;      // 25.0 dB
    bool tunerAgc = false;
    bool rtlAgc = false;
    int frequencyCorrectionPpm = 0;
    int directSampling = 0;          // 0: Off, 1: I-ADC, 2: Q-ADC
};

struct SdrStats {
    bool connected = false;
    std::string deviceName = "RTL-SDR (Disconnected)";
    std::string tunerType = "Unknown";
    uint64_t bytesReceived = 0;
    float currentKBytesPerSec = 0.0f;
    uint32_t bufferOverruns = 0;
    uint32_t droppedPackets = 0;
};

} // namespace RTLSDR
