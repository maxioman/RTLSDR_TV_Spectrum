#include "MainWindow.hpp"
#include <imgui.h>
#include <implot.h>

#if defined(__APPLE__)
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif

#include <cmath>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <iostream>

namespace RTLSDR {

MainWindow::MainWindow()
    : spectrumEngine_(1024, 256)
{
    // Initialize default SDR device to test generator initially
    sdrDevice_ = std::make_unique<TestSignalGenerator>();
    sdrDevice_->setCallback([this](const uint8_t* data, size_t len) {
        handleSdrSamples(data, len);
    });

    config_.frequencyHz = 175250000; // 175.25 MHz (VHF CH E5 PAL Video Carrier)
    config_.sampleRateHz = 2400000;  // 2.4 MSPS
    sdrDevice_->open(config_);
}

MainWindow::~MainWindow() {
    disconnectDevice();
    if (tvTextureId_ != 0) {
        glDeleteTextures(1, &tvTextureId_);
        tvTextureId_ = 0;
    }
    if (waterfallTextureId_ != 0) {
        glDeleteTextures(1, &waterfallTextureId_);
        waterfallTextureId_ = 0;
    }
}

void MainWindow::handleSdrSamples(const uint8_t* data, size_t length) {
    // Feed samples simultaneously to Spectrum Engine and Analog TV Demodulator
    spectrumEngine_.pushIqBytes(data, length, static_cast<double>(config_.frequencyHz), static_cast<double>(config_.sampleRateHz));
    tvDemod_.processIqBytes(data, length);
}

void MainWindow::connectDevice() {
    disconnectDevice();

    config_.remoteHost = hostInput_;
    config_.remotePort = static_cast<uint16_t>(portInput_);
    config_.frequencyHz = static_cast<uint32_t>(std::round(centerFreqMHz_ * 1e6));
    config_.tunerGainTenthsDb = gainTenthsDb_;
    config_.tunerAgc = tunerAgc_;
    config_.rtlAgc = rtlAgc_;

    const uint32_t sampleRates[] = { 1024000, 2048000, 2400000, 2560000, 2880000, 3200000 };
    if (sampleRateIdx_ >= 0 && sampleRateIdx_ < 6) {
        config_.sampleRateHz = sampleRates[sampleRateIdx_];
    }
    tvDemod_.setSampleRate(config_.sampleRateHz);

    if (selectedSource_ == 0) {
        sdrDevice_ = std::make_unique<RtlTcpClient>();
    } else if (selectedSource_ == 1) {
        sdrDevice_ = std::make_unique<RtlUsbDevice>();
    } else {
        sdrDevice_ = std::make_unique<TestSignalGenerator>();
    }

    sdrDevice_->setCallback([this](const uint8_t* data, size_t len) {
        handleSdrSamples(data, len);
    });

    sdrDevice_->open(config_);
    tvDemod_.clearFrame();
}

void MainWindow::disconnectDevice() {
    if (sdrDevice_) {
        sdrDevice_->close();
    }
}

void MainWindow::tuneToFrequency(uint32_t freqHz) {
    config_.frequencyHz = freqHz;
    centerFreqMHz_ = static_cast<double>(freqHz) / 1e6;
    tvDemod_.clearFrame();
    if (sdrDevice_ && sdrDevice_->isRunning()) {
        sdrDevice_->setFrequency(freqHz);
    }
}

void MainWindow::render() {
    renderTopBar();

    ImGui::Separator();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("MainAppTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("📺 Analog TV Viewer (TVSharper)")) {
            renderAnalogTvTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("📊 VHF / UHF Spectrum & Waterfall")) {
            renderSpectrumTab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("⚙️ SDR Diagnostics & rtl_tcp")) {
            renderDiagnosticsTab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void MainWindow::renderTopBar() {
    SdrStats stats = sdrDevice_ ? sdrDevice_->getStats() : SdrStats{};

    // Source combo
    const char* sources[] = { "rtl_tcp Network (Raspberry Pi 4)", "Local USB RTL-SDR", "Synthetic RF Pattern Gen" };
    ImGui::SetNextItemWidth(260.0f);
    if (ImGui::Combo("SDR Source", &selectedSource_, sources, IM_ARRAYSIZE(sources))) {
        connectDevice();
    }

    ImGui::SameLine(0.0f, 15.0f);

    // Host & Port (if rtl_tcp)
    if (selectedSource_ == 0) {
        ImGui::SetNextItemWidth(130.0f);
        ImGui::InputText("Host", hostInput_, sizeof(hostInput_));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(65.0f);
        ImGui::InputInt("Port", &portInput_, 0, 0);
        ImGui::SameLine();
    }

    // Connect / Disconnect Button
    if (stats.connected) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
        if (ImGui::Button("DISCONNECT")) {
            disconnectDevice();
        }
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.25f, 1.0f));
        if (ImGui::Button("CONNECT")) {
            connectDevice();
        }
        ImGui::PopStyleColor();
    }

    ImGui::SameLine(0.0f, 15.0f);

    // Connection indicator badge
    if (stats.connected) {
        ImGui::TextColored(ImVec4(0.1f, 1.0f, 0.3f, 1.0f), "● ONLINE (%.1f KB/s)", stats.currentKBytesPerSec);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "○ OFFLINE");
    }

    ImGui::Spacing();

    // Tuning Controls Row
    ImGui::PushItemWidth(140.0f);
    if (ImGui::InputDouble("Center (MHz)", &centerFreqMHz_, 0.05, 1.0, "%.4f")) {
        tuneToFrequency(static_cast<uint32_t>(std::round(centerFreqMHz_ * 1e6)));
    }
    ImGui::PopItemWidth();

    // Quick Step Buttons
    ImGui::SameLine();
    if (ImGui::Button("-1M"))   { centerFreqMHz_ -= 1.0;    tuneToFrequency(static_cast<uint32_t>(std::round(centerFreqMHz_ * 1e6))); }
    ImGui::SameLine();
    if (ImGui::Button("-100k")) { centerFreqMHz_ -= 0.1;    tuneToFrequency(static_cast<uint32_t>(std::round(centerFreqMHz_ * 1e6))); }
    ImGui::SameLine();
    if (ImGui::Button("+100k")) { centerFreqMHz_ += 0.1;    tuneToFrequency(static_cast<uint32_t>(std::round(centerFreqMHz_ * 1e6))); }
    ImGui::SameLine();
    if (ImGui::Button("+1M"))   { centerFreqMHz_ += 1.0;    tuneToFrequency(static_cast<uint32_t>(std::round(centerFreqMHz_ * 1e6))); }

    ImGui::SameLine(0.0f, 25.0f);

    // Sample Rate
    const char* rates[] = { "1.024 MSPS", "2.048 MSPS", "2.400 MSPS (TV)", "2.560 MSPS", "2.880 MSPS", "3.200 MSPS" };
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Sample Rate", &sampleRateIdx_, rates, IM_ARRAYSIZE(rates))) {
        connectDevice();
    }

    ImGui::SameLine(0.0f, 15.0f);

    // Gain Slider & AGC
    if (tunerAgc_) {
        ImGui::BeginDisabled();
        float dummyGain = 25.0f;
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderFloat("Gain", &dummyGain, 0.0f, 49.6f, "Auto AGC");
        ImGui::EndDisabled();
    } else {
        float gainDb = static_cast<float>(gainTenthsDb_) / 10.0f;
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::SliderFloat("Gain (dB)", &gainDb, 0.0f, 49.6f, "%.1f dB")) {
            gainTenthsDb_ = static_cast<int>(std::round(gainDb * 10.0f));
            if (sdrDevice_) sdrDevice_->setGain(gainTenthsDb_);
        }
    }

    ImGui::SameLine();
    if (ImGui::Checkbox("Tuner AGC", &tunerAgc_)) {
        if (sdrDevice_) sdrDevice_->setTunerAgc(tunerAgc_);
    }
}

void MainWindow::renderAnalogTvTab() {
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    // Two-column layout: Left = TV CRT Display, Right = Controls Rack
    ImGui::Columns(2, "TvColumns", true);
    ImGui::SetColumnWidth(0, availW * 0.65f);

    // 1. LEFT COLUMN: TV Display
    ImGui::BeginChild("TvDisplayArea", ImVec2(0, 0), true);

    // Fetch decoded frame from demodulator
    std::vector<uint32_t> rgbaPixels;
    int frameW = 0, frameH = 0;
    bool hasFrame = tvDemod_.getFrame(rgbaPixels, frameW, frameH);

    // Update OpenGL Texture
    if (hasFrame && frameW > 0 && frameH > 0) {
        if (tvTextureId_ == 0) {
            glGenTextures(1, &tvTextureId_);
            glBindTexture(GL_TEXTURE_2D, tvTextureId_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frameW, frameH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels.data());
            tvTexWidth_ = frameW;
            tvTexHeight_ = frameH;
        } else if (tvTexWidth_ != frameW || tvTexHeight_ != frameH) {
            glBindTexture(GL_TEXTURE_2D, tvTextureId_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, frameW, frameH, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels.data());
            tvTexWidth_ = frameW;
            tvTexHeight_ = frameH;
        } else {
            glBindTexture(GL_TEXTURE_2D, tvTextureId_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frameW, frameH, GL_RGBA, GL_UNSIGNED_BYTE, rgbaPixels.data());
        }
    }

    TvStats st = tvDemod_.getStats();
    TvConfig curCfg = tvDemod_.getConfig();

    // Top OSD Header
    ImGui::TextColored(st.hasSyncLock ? ImVec4(0.2f, 1.0f, 0.4f, 1.0f) : ImVec4(1.0f, 0.4f, 0.2f, 1.0f),
        "%s | %s (%.2f MHz) | %s",
        st.hasSyncLock ? "🟢 SYNC LOCKED" : "⚪ NO SYNC LOCK",
        curCfg.standard == TvStandard::NTSC_M ? "NTSC 525/60" : "PAL 625/50",
        centerFreqMHz_,
        curCfg.inverseVideo ? "Positive Video (SECAM L)" : "Negative Video (PAL/NTSC)");

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 120.0f);
    ImGui::TextColored(ImVec4(0.8f, 0.85f, 0.9f, 1.0f), "FPS: %.1f", st.frameRateFps);

    // Render TV screen rectangle with 4:3 Aspect Ratio
    float screenAvailW = ImGui::GetContentRegionAvail().x - 10.0f;
    float screenAvailH = ImGui::GetContentRegionAvail().y - 30.0f;
    float displayW = screenAvailW;
    float displayH = displayW * (3.0f / 4.0f); // 4:3 classic TV
    if (displayH > screenAvailH) {
        displayH = screenAvailH;
        displayW = displayH * (4.0f / 3.0f);
    }

    ImVec2 screenP0 = ImGui::GetCursorScreenPos();
    ImVec2 screenP1(screenP0.x + displayW, screenP0.y + displayH);
    ImDrawList* drawList = ImGui::GetWindowDrawList();

    // Outer CRT bezel
    drawList->AddRectFilled(ImVec2(screenP0.x - 4, screenP0.y - 4), ImVec2(screenP1.x + 4, screenP1.y + 4), IM_COL32(20, 24, 30, 255), 6.0f);
    drawList->AddRect(ImVec2(screenP0.x - 4, screenP0.y - 4), ImVec2(screenP1.x + 4, screenP1.y + 4), IM_COL32(60, 75, 95, 255), 6.0f, 0, 2.0f);

    if (tvTextureId_ != 0) {
        ImGui::Image(static_cast<ImTextureID>(tvTextureId_), ImVec2(displayW, displayH));
    } else {
        drawList->AddRectFilled(screenP0, screenP1, IM_COL32(0, 0, 0, 255));
        drawList->AddText(ImVec2(screenP0.x + displayW * 0.35f, screenP0.y + displayH * 0.45f), IM_COL32(200, 210, 220, 255), "NO VIDEO SIGNAL");
        ImGui::Dummy(ImVec2(displayW, displayH));
    }

    ImGui::EndChild();

    ImGui::NextColumn();

    // 2. RIGHT COLUMN: Control Rack & Oscilloscope
    ImGui::BeginChild("TvControlRack", ImVec2(0, 0), true);

    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "ANALOG TV CONTROLS");
    ImGui::Separator();

    TvConfig cfg = tvDemod_.getConfig();

    // TV Standard Selector
    const char* tvStandards[] = {
        "PAL B/G (625 lines / 50 Hz, 5.5M Audio)",
        "PAL I (625 lines / 50 Hz, 6.0M Audio)",
        "PAL D/K (625 lines / 50 Hz, 6.5M Audio)",
        "NTSC M (525 lines / 60 Hz, 4.5M Audio)",
        "SECAM L (Positive Video, 6.5M Audio)"
    };
    int stdIdx = static_cast<int>(cfg.standard);
    if (ImGui::Combo("Standard", &stdIdx, tvStandards, IM_ARRAYSIZE(tvStandards))) {
        cfg.standard = static_cast<TvStandard>(stdIdx);
        tvDemod_.setConfig(cfg);
    }

    ImGui::Spacing();

    // Pattern & Image Transmission Controls (when source is Synthetic RF Gen)
    auto* gen = dynamic_cast<TestSignalGenerator*>(sdrDevice_.get());
    if (selectedSource_ == 2 || gen != nullptr) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.7f, 1.0f), "📺 SYNTHETIC TRANSMITTER & TESTCARD");

        const char* patternNames[] = {
            "Philips PM5544 Testcard (Broadcast Standard)",
            "EBU / SMPTE Colorbars",
            "Custom Image File (PNG / JPG / BMP)"
        };
        if (ImGui::Combo("Test Pattern", &selectedPatternType_, patternNames, IM_ARRAYSIZE(patternNames))) {
            if (gen) {
                gen->setPatternType(static_cast<TestPatternType>(selectedPatternType_));
            }
        }

        if (selectedPatternType_ == 2) {
            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100.0f);
            ImGui::InputTextWithHint("##ImagePath", "Path to .png / .jpg image...", customImagePath_, sizeof(customImagePath_));
            ImGui::SameLine();
            if (ImGui::Button("Load Image", ImVec2(90, 0))) {
                if (gen && customImagePath_[0] != '\0') {
                    if (gen->loadCustomImage(customImagePath_)) {
                        customImageStatus_ = gen->getLoadedImageInfo();
                    } else {
                        customImageStatus_ = "Error: File not found or unreadable";
                    }
                }
            }
            ImGui::TextDisabled("Status: %s", customImageStatus_.c_str());
        }

        if (ImGui::SliderFloat("RF Static Noise", &rfNoiseLevel_, 0.0f, 0.25f, "%.2f (Static)")) {
            if (gen) {
                gen->setNoiseLevel(rfNoiseLevel_);
            }
        }
        ImGui::Separator();
        ImGui::Spacing();
    }

    // Channel Presets
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "VHF DIRECT CHANNELS (CH 1 - 12)");

    struct VhfChannel {
        int ch;
        const char* name;
        uint32_t freqHz;
    };
    static const VhfChannel vhfChannels[12] = {
        { 1,  "CH 1 (45.00M)",  45000000 },
        { 2,  "CH 2 (48.25M)",  48250000 },
        { 3,  "CH 3 (55.25M)",  55250000 },
        { 4,  "CH 4 (62.25M)",  62250000 },
        { 5,  "CH 5 (175.25M)", 175250000 },
        { 6,  "CH 6 (182.25M)", 182250000 },
        { 7,  "CH 7 (189.25M)", 189250000 },
        { 8,  "CH 8 (196.25M)", 196250000 },
        { 9,  "CH 9 (203.25M)", 203250000 },
        { 10, "CH 10 (210.25M)", 210250000 },
        { 11, "CH 11 (217.25M)", 217250000 },
        { 12, "CH 12 (224.25M)", 224250000 },
    };

    float vhfBtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3.0f) / 4.0f;
    if (vhfBtnW < 75.0f) vhfBtnW = 75.0f;

    for (int i = 0; i < 12; ++i) {
        bool isActive = (std::abs(centerFreqMHz_ - (vhfChannels[i].freqHz / 1e6)) < 0.05);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.35f, 1.0f));
        }
        if (ImGui::Button(vhfChannels[i].name, ImVec2(vhfBtnW, 0))) {
            tuneToFrequency(vhfChannels[i].freqHz);
        }
        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        if ((i % 4) != 3) {
            ImGui::SameLine();
        }
    }

    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.2f, 1.0f), "UHF CHANNELS (BAND IV / V)");

    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::InputInt("##UhfStartInput", &uhfStartChannel_, 1, 5)) {
        uhfStartChannel_ = std::clamp(uhfStartChannel_, 14, 64);
    }
    ImGui::SameLine();
    if (ImGui::Button("< -5")) {
        uhfStartChannel_ = std::max(14, uhfStartChannel_ - 5);
    }
    ImGui::SameLine();
    if (ImGui::Button("+5 >")) {
        uhfStartChannel_ = std::min(64, uhfStartChannel_ + 5);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Start: CH %d (Range %d - %d)", uhfStartChannel_, uhfStartChannel_, uhfStartChannel_ + 5);

    float uhfBtnW = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (uhfBtnW < 90.0f) uhfBtnW = 90.0f;

    for (int i = 0; i < 6; ++i) {
        int ch = uhfStartChannel_ + i;
        double freqMHz = 471.25 + (ch - 21) * 8.0;
        uint32_t freqHz = static_cast<uint32_t>(std::round(freqMHz * 1e6));

        char btnLabel[48];
        std::snprintf(btnLabel, sizeof(btnLabel), "CH %d (%.2fM)", ch, freqMHz);

        bool isActive = (std::abs(centerFreqMHz_ - freqMHz) < 0.05);
        if (isActive) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.65f, 0.3f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.2f, 0.75f, 0.35f, 1.0f));
        }
        if (ImGui::Button(btnLabel, ImVec2(uhfBtnW, 0))) {
            tuneToFrequency(freqHz);
        }
        if (isActive) {
            ImGui::PopStyleColor(2);
        }
        if ((i % 3) != 2 && i < 5) {
            ImGui::SameLine();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Video adjustments
    if (ImGui::SliderInt("Brightness", &cfg.brightness, -100, 100)) {
        tvDemod_.setConfig(cfg);
    }
    if (ImGui::SliderFloat("Contrast", &cfg.contrast, 0.2f, 3.0f, "%.2fx")) {
        tvDemod_.setConfig(cfg);
    }
    if (ImGui::Checkbox("Inverse Video Polarity", &cfg.inverseVideo)) {
        tvDemod_.setConfig(cfg);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Sync Controls
    if (ImGui::Checkbox("Deinterlace (Progressive)", &cfg.deinterlace)) {
        tvDemod_.setConfig(cfg);
    }
    ImGui::SameLine();
    if (ImGui::Checkbox("Auto-Sync Lock", &cfg.autoSync)) {
        tvDemod_.setConfig(cfg);
    }
    if (!cfg.autoSync) {
        if (ImGui::SliderInt("Phase X", &cfg.manualX, -100, 100)) {
            tvDemod_.setConfig(cfg);
        }
        if (ImGui::SliderInt("Phase Y", &cfg.manualY, -100, 100)) {
            tvDemod_.setConfig(cfg);
        }
    }
    if (ImGui::SliderInt("Fine Line Clock", &cfg.fineFrequencyCorrection, -100, 100)) {
        tvDemod_.setConfig(cfg);
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Mini Scanline Waveform (IRE Scope)
    ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.5f, 1.0f), "VIDEO LINE SCOPE (IRE)");
    std::vector<float> waveform;
    float syncLevel = 90.0f, blackLevel = 80.0f;
    tvDemod_.getWaveform(waveform, syncLevel, blackLevel);

    if (ImPlot::BeginPlot("##ScanlinePlot", ImVec2(-1, 140), ImPlotFlags_NoLegend | ImPlotFlags_NoMouseText)) {
        ImPlot::SetupAxes("Sample", "Level", ImPlotAxisFlags_None, ImPlotAxisFlags_None);
        ImPlot::SetupAxesLimits(0, waveform.size(), 0, 160, ImPlotCond_Always);

        // Blanking and sync threshold lines
        double syncY = syncLevel;
        double blackY = blackLevel;
        ImPlotSpec sSpec; sSpec.LineColor = ImVec4(1.0f, 0.2f, 0.2f, 0.7f);
        ImPlot::PlotInfLines("Sync Thresh", &syncY, 1, sSpec);

        ImPlotSpec bSpec; bSpec.LineColor = ImVec4(0.2f, 0.8f, 1.0f, 0.7f);
        ImPlot::PlotInfLines("Black Level", &blackY, 1, bSpec);

        if (!waveform.empty()) {
            std::vector<double> xs(waveform.size());
            std::vector<double> ys(waveform.size());
            for (size_t i = 0; i < waveform.size(); ++i) {
                xs[i] = static_cast<double>(i);
                ys[i] = static_cast<double>(waveform[i]);
            }
            ImPlotSpec wSpec;
            wSpec.LineColor = ImVec4(0.1f, 1.0f, 0.4f, 1.0f);
            wSpec.LineWeight = 1.8f;
            ImPlot::PlotLine("##Wave", xs.data(), ys.data(), static_cast<int>(waveform.size()), wSpec);
        }
        ImPlot::EndPlot();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // Audio Subcarrier Controls
    ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f), "AUDIO SUBCARRIER");
    if (ImGui::Checkbox("Enable Sound Demod", &cfg.audioEnabled)) {
        tvDemod_.setConfig(cfg);
    }
    if (cfg.audioEnabled) {
        if (ImGui::SliderFloat("Volume", &cfg.audioVolume, 0.0f, 1.0f, "%.0f%%")) {
            tvDemod_.setConfig(cfg);
        }
    }

    ImGui::EndChild();

    ImGui::Columns(1);
}

void MainWindow::renderSpectrumTab() {
    float availW = ImGui::GetContentRegionAvail().x;
    float availH = ImGui::GetContentRegionAvail().y;

    SpectrumSnapshot snap;
    bool hasSnap = spectrumEngine_.getSnapshot(snap);

    // 1. RF POWER SPECTRUM (Top Half)
    float plotH = availH * 0.52f;
    if (ImPlot::BeginPlot("##RfPowerSpectrum", ImVec2(availW, plotH))) {
        ImPlot::SetupAxes("Frequency (MHz)", "Power (dBFS)", ImPlotAxisFlags_None, ImPlotAxisFlags_None);

        if (hasSnap && !snap.frequenciesMHz.empty()) {
            double startF = snap.frequenciesMHz.front();
            double endF = snap.frequenciesMHz.back();
            ImPlot::SetupAxesLimits(startF, endF, -110.0, 0.0, ImPlotCond_Always);

            std::vector<double> xs(snap.frequenciesMHz.begin(), snap.frequenciesMHz.end());
            std::vector<double> curY(snap.currentDbfs.begin(), snap.currentDbfs.end());
            std::vector<double> maxY(snap.maxHoldDbfs.begin(), snap.maxHoldDbfs.end());
            std::vector<double> avgY(snap.avgDbfs.begin(), snap.avgDbfs.end());

            // Max hold trace (cyan)
            ImPlotSpec maxSpec;
            maxSpec.LineColor = ImVec4(0.2f, 0.8f, 0.9f, 0.6f);
            maxSpec.LineWeight = 1.0f;
            ImPlot::PlotLine("Max Hold", xs.data(), maxY.data(), static_cast<int>(xs.size()), maxSpec);

            // Yellow fill under live trace
            ImPlotSpec fillSpec;
            fillSpec.FillColor = ImVec4(1.0f, 0.85f, 0.1f, 0.25f);
            ImPlot::PlotShaded("##Fill", xs.data(), curY.data(), static_cast<int>(xs.size()), -120.0, fillSpec);

            // Canary yellow live curve
            ImPlotSpec liveSpec;
            liveSpec.LineColor = ImVec4(1.0f, 0.85f, 0.1f, 1.0f);
            liveSpec.LineWeight = 1.8f;
            ImPlot::PlotLine("Live Trace", xs.data(), curY.data(), static_cast<int>(xs.size()), liveSpec);

            // Center tuned cursor (red)
            double curF = centerFreqMHz_;
            ImPlotSpec cSpec;
            cSpec.LineColor = ImVec4(0.95f, 0.15f, 0.15f, 0.9f);
            cSpec.LineWeight = 2.0f;
            ImPlot::PlotInfLines("Center Freq", &curF, 1, cSpec);

            // Peak Marker readout
            char peakBuf[64];
            snprintf(peakBuf, sizeof(peakBuf), "Peak: %.4f MHz (%.1f dBFS)", snap.peakFreqMHz, snap.peakDbfs);
            ImPlotSpec pSpec;
            pSpec.LineColor = ImVec4(1.0f, 0.95f, 0.3f, 1.0f);
            ImPlot::PlotText(peakBuf, snap.peakFreqMHz, snap.peakDbfs + 6.0, ImVec2(0, 0), pSpec);
        }

        ImPlot::EndPlot();
    }

    ImGui::Spacing();

    // 2. 2D WATERFALL SPECTROGRAM (Bottom Half)
    float waterfallH = ImGui::GetContentRegionAvail().y - 38.0f;

    // Update Waterfall Texture
    size_t wW = spectrumEngine_.getWaterfallWidth();
    size_t wH = spectrumEngine_.getWaterfallHeight();
    const auto& wPixels = spectrumEngine_.getWaterfallPixels();

    if (wW > 0 && wH > 0 && !wPixels.empty()) {
        if (waterfallTextureId_ == 0) {
            glGenTextures(1, &waterfallTextureId_);
            glBindTexture(GL_TEXTURE_2D, waterfallTextureId_);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<int>(wW), static_cast<int>(wH), 0, GL_RGBA, GL_UNSIGNED_BYTE, wPixels.data());
            waterfallTexWidth_ = wW;
            waterfallTexHeight_ = wH;
        } else if (waterfallTexWidth_ != wW || waterfallTexHeight_ != wH) {
            glBindTexture(GL_TEXTURE_2D, waterfallTextureId_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, static_cast<int>(wW), static_cast<int>(wH), 0, GL_RGBA, GL_UNSIGNED_BYTE, wPixels.data());
            waterfallTexWidth_ = wW;
            waterfallTexHeight_ = wH;
        } else if (spectrumEngine_.isWaterfallDirty()) {
            glBindTexture(GL_TEXTURE_2D, waterfallTextureId_);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<int>(wW), static_cast<int>(wH), GL_RGBA, GL_UNSIGNED_BYTE, wPixels.data());
            spectrumEngine_.clearWaterfallDirty();
        }
    }

    if (waterfallTextureId_ != 0) {
        ImGui::Image(static_cast<ImTextureID>(waterfallTextureId_), ImVec2(availW, waterfallH));
    } else {
        ImGui::Dummy(ImVec2(availW, waterfallH));
    }

    // Spectrum Controls Bar
    if (ImGui::Button("Reset Max Hold")) {
        spectrumEngine_.resetMaxHold();
    }
    ImGui::SameLine(0.0f, 15.0f);

    const char* palettes[] = { "Turbo (Rainbow)", "Viridis", "Plasma", "Fire" };
    static int curPalette = 0;
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::Combo("Palette", &curPalette, palettes, IM_ARRAYSIZE(palettes))) {
        spectrumEngine_.setWaterfallPalette(static_cast<WaterfallPalette>(curPalette));
    }

    ImGui::SameLine(0.0f, 20.0f);
    ImGui::TextColored(ImVec4(0.3f, 0.85f, 1.0f, 1.0f), "VHF/UHF Presets:");
    ImGui::SameLine();
    if (ImGui::Button("FM Radio (98M)"))      tuneToFrequency(98000000);
    ImGui::SameLine();
    if (ImGui::Button("Airband (128M)"))       tuneToFrequency(128000000);
    ImGui::SameLine();
    if (ImGui::Button("VHF III (190M)"))      tuneToFrequency(190000000);
    ImGui::SameLine();
    if (ImGui::Button("ISM 433M"))             tuneToFrequency(433920000);
    ImGui::SameLine();
    if (ImGui::Button("UHF TV (550M)"))       tuneToFrequency(550000000);
    ImGui::SameLine();
    if (ImGui::Button("LTE-800 (806M)"))       tuneToFrequency(806000000);
}

void MainWindow::renderDiagnosticsTab() {
    SdrStats stats = sdrDevice_ ? sdrDevice_->getStats() : SdrStats{};

    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "RTL-SDR DEVICE & NETWORK DIAGNOSTICS");
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::Text("Device Name:          %s", stats.deviceName.c_str());
    ImGui::Text("Tuner Chip:           %s", stats.tunerType.c_str());
    ImGui::Text("Connection Status:    %s", stats.connected ? "CONNECTED" : "DISCONNECTED");
    ImGui::Text("Total Bytes Received: %llu bytes (%.2f MB)", stats.bytesReceived, static_cast<double>(stats.bytesReceived) / (1024.0 * 1024.0));
    ImGui::Text("Stream Throughput:    %.2f KB/s (%.2f Mbit/s)", stats.currentKBytesPerSec, (stats.currentKBytesPerSec * 8.0f) / 1024.0f);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "RASPBERRY PI 4 SETUP INSTRUCTIONS (manohar@192.168.1.27):");
    ImGui::BulletText("To start rtl_tcp on your Raspberry Pi 4, run:");
    ImGui::Indent();
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "rtl_tcp -a 0.0.0.0 -p 1234");
    ImGui::Unindent();
    ImGui::BulletText("Then from your Mac or local viewer, connect to 192.168.1.27 port 1234.");
    ImGui::BulletText("To cross-compile and run this native GUI directly on Raspberry Pi 4 HDMI:");
    ImGui::Indent();
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "./build_pi4.sh && scp build_pi4/RtlSdrTvSpectrum manohar@192.168.1.27:~/");
    ImGui::Unindent();
}

} // namespace RTLSDR
