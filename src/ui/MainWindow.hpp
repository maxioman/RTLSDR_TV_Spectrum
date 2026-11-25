#pragma once

#include "sdr/ISdrDevice.hpp"
#include "sdr/RtlTcpClient.hpp"
#include "sdr/RtlUsbDevice.hpp"
#include "sdr/TestSignalGenerator.hpp"
#include "dsp/SpectrumEngine.hpp"
#include "dsp/AnalogTvDemod.hpp"

#include <memory>
#include <string>
#include <vector>

namespace RTLSDR {

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    void render();

private:
    void renderTopBar();
    void renderAnalogTvTab();
    void renderSpectrumTab();
    void renderDiagnosticsTab();

    void handleSdrSamples(const uint8_t* data, size_t length);
    void connectDevice();
    void disconnectDevice();
    void tuneToFrequency(uint32_t freqHz);

    // SDR Subsystem
    SdrConfig config_;
    std::unique_ptr<ISdrDevice> sdrDevice_;
    SpectrumEngine spectrumEngine_;
    AnalogTvDemod tvDemod_;

    // UI State
    int selectedSource_ = 0; // 0: rtl_tcp, 1: Local USB, 2: Test Gen
    char hostInput_[64] = "192.168.1.27";
    int portInput_ = 1234;
    double centerFreqMHz_ = 175.250; // CH E5 PAL Video Carrier
    int sampleRateIdx_ = 2;          // 2.4 MSPS
    int gainTenthsDb_ = 250;         // 25.0 dB
    bool tunerAgc_ = false;
    bool rtlAgc_ = false;
    int uhfStartChannel_ = 20;       // Starting channel for UHF dynamic buttons

    // Pattern Generator controls (Synthetic RF Source)
    int selectedPatternType_ = 0;    // 0: PM5544 Testcard, 1: EBU Colorbars, 2: Custom Image
    char customImagePath_[512] = "test_pattern.bmp";
    float rfNoiseLevel_ = 0.04f;
    std::string customImageStatus_ = "Ready to load: test_pattern.bmp";

    // OpenGL Textures for TV and Waterfall
    unsigned int tvTextureId_ = 0;
    int tvTexWidth_ = 0;
    int tvTexHeight_ = 0;

    unsigned int waterfallTextureId_ = 0;
    size_t waterfallTexWidth_ = 0;
    size_t waterfallTexHeight_ = 0;

    // Audio output
    bool audioDeviceOpened_ = false;
};

} // namespace RTLSDR
