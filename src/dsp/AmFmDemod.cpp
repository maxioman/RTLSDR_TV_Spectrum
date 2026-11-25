#include "AmFmDemod.hpp"

#include <algorithm>
#include <cmath>

namespace RTLSDR {

static constexpr double kAudioSampleRate = 48000.0;

AmFmDemod::AmFmDemod()
    : audioRingBuffer_(48000, 0.0f)
    , waveformBuffer_(512, 0.0f)
{
}

void AmFmDemod::setConfig(const AmFmConfig& cfg) {
    std::lock_guard<std::mutex> lock(configMutex_);
    config_ = cfg;
}

AmFmConfig AmFmDemod::getConfig() const {
    std::lock_guard<std::mutex> lock(configMutex_);
    return config_;
}

void AmFmDemod::reset() {
    std::lock_guard<std::mutex> aLock(audioMutex_);
    audioReadIdx_ = 0;
    audioWriteIdx_ = 0;
    ncoPhase_ = 0.0;
    lpf1I_ = lpf1Q_ = lpf2I_ = lpf2Q_ = 0.0f;
    prevIfI_ = prevIfQ_ = 0.0f;
    amDcPrevIn_ = amDcPrevOut_ = 0.0f;
    amAgcEnv_ = 0.1f;
    deemphState_ = 0.0f;
    audioAccum_ = 0.0;
    audioSum_ = 0.0f;
    audioCount_ = 0;
}

void AmFmDemod::processIqBytes(const uint8_t* iqData, size_t length, double centerFreqHz, double sampleRateHz) {
    if (!iqData || length < 2 || sampleRateHz <= 0.0) return;

    AmFmConfig cfg;
    {
        std::lock_guard<std::mutex> lock(configMutex_);
        cfg = config_;
    }

    const size_t numSamples = length / 2;

    // Frequency shift NCO step
    const double ncoStep = -2.0 * M_PI * cfg.channelOffsetHz / sampleRateHz;

    // 2-stage single-pole IIR low-pass coefficient for channel selectivity
    const float cutoffHz = std::max(1000.0f, std::min(static_cast<float>(sampleRateHz * 0.45), cfg.bandwidthHz * 0.5f));
    const float alphaLp = 1.0f - std::exp(-2.0f * static_cast<float>(M_PI) * cutoffHz / static_cast<float>(sampleRateHz));

    // Intermediate IF rate for demodulation (~240 kHz for WFM, ~48 kHz for AM/NFM)
    const double targetIfRate = (cfg.mode == DemodMode::WFM) ? 240000.0 : 48000.0;
    const double ifStep = std::max(1.0, sampleRateHz / targetIfRate);
    const double actualIfRate = sampleRateHz / ifStep;

    // De-emphasis coefficient at actualIfRate
    float deemphAlpha = 1.0f;
    if (cfg.deemphasisUs > 1.0f && cfg.mode != DemodMode::AM) {
        double tau = static_cast<double>(cfg.deemphasisUs) * 1e-6;
        deemphAlpha = static_cast<float>(1.0 - std::exp(-1.0 / (actualIfRate * tau)));
    }

    const float fmMaxDevHz = (cfg.mode == DemodMode::WFM) ? 75000.0f : 5000.0f;
    const float fmNormGain = static_cast<float>(actualIfRate) / (2.0f * static_cast<float>(M_PI) * fmMaxDevHz);

    const double audioStep = actualIfRate / kAudioSampleRate;
    const float rssiAlpha = 1.0f - std::exp(-1.0f / static_cast<float>(actualIfRate * 0.05));

    for (size_t i = 0; i < numSamples; ++i) {
        const float rawI = (static_cast<float>(iqData[i * 2 + 0]) - 127.5f) / 128.0f;
        const float rawQ = (static_cast<float>(iqData[i * 2 + 1]) - 127.5f) / 128.0f;

        // Frequency shift to baseband
        float mixI = rawI;
        float mixQ = rawQ;
        if (std::abs(cfg.channelOffsetHz) > 1.0) {
            const float c = static_cast<float>(std::cos(ncoPhase_));
            const float s = static_cast<float>(std::sin(ncoPhase_));
            mixI = rawI * c - rawQ * s;
            mixQ = rawI * s + rawQ * c;
            ncoPhase_ += ncoStep;
            if (ncoPhase_ > M_PI) ncoPhase_ -= 2.0 * M_PI;
            if (ncoPhase_ < -M_PI) ncoPhase_ += 2.0 * M_PI;
        }

        // 2-stage cascaded low-pass filter
        lpf1I_ += alphaLp * (mixI - lpf1I_);
        lpf1Q_ += alphaLp * (mixQ - lpf1Q_);
        lpf2I_ += alphaLp * (lpf1I_ - lpf2I_);
        lpf2Q_ += alphaLp * (lpf1Q_ - lpf2Q_);

        ifSampleAccum_ += 1.0;
        if (ifSampleAccum_ < ifStep) {
            continue;
        }
        ifSampleAccum_ -= ifStep;

        const float ifI = lpf2I_;
        const float ifQ = lpf2Q_;

        // Channel RSSI power estimate
        const float instPow = ifI * ifI + ifQ * ifQ;
        powerAvg_ += rssiAlpha * (instPow - powerAvg_);
        const float rssiDbfs = 10.0f * std::log10(std::max(1e-12f, powerAvg_));

        const bool squelchOpen = (rssiDbfs >= cfg.squelchDbfs);
        const float targetSq = squelchOpen ? 1.0f : 0.0f;
        squelchGain_ += 0.02f * (targetSq - squelchGain_);

        float demodSample = 0.0f;
        if (cfg.mode == DemodMode::AM) {
            const float env = std::sqrt(instPow);
            // DC-blocking high-pass filter: y[n] = x[n] - x[n-1] + 0.995 * y[n-1]
            const float dcBlocked = env - amDcPrevIn_ + 0.995f * amDcPrevOut_;
            amDcPrevIn_ = env;
            amDcPrevOut_ = dcBlocked;

            // Slow AGC on carrier envelope
            amAgcEnv_ += 0.001f * (env - amAgcEnv_);
            const float agcGain = 0.45f / std::max(0.01f, amAgcEnv_);
            demodSample = dcBlocked * agcGain;
        } else {
            // Quadrature FM discriminator: arg(z[n] * conj(z[n-1]))
            const float dot = ifI * prevIfI_ + ifQ * prevIfQ_;
            const float cross = ifQ * prevIfI_ - ifI * prevIfQ_;
            prevIfI_ = ifI;
            prevIfQ_ = ifQ;

            const float dphi = std::atan2(cross, dot);
            demodSample = dphi * fmNormGain;

            if (cfg.deemphasisUs > 1.0f) {
                deemphState_ += deemphAlpha * (demodSample - deemphState_);
                demodSample = deemphState_;
            }
        }

        // Accumulate and decimate to 48 kHz audio
        audioSum_ += demodSample;
        audioCount_ += 1;
        audioAccum_ += 1.0;

        if (audioAccum_ >= audioStep) {
            audioAccum_ -= audioStep;
            const float avgSample = (audioCount_ > 0) ? (audioSum_ / static_cast<float>(audioCount_)) : 0.0f;
            audioSum_ = 0.0f;
            audioCount_ = 0;

            const float pcm = std::max(-1.0f, std::min(1.0f, avgSample * squelchGain_ * cfg.volume));

            if (cfg.audioEnabled) {
                std::lock_guard<std::mutex> aLock(audioMutex_);
                audioRingBuffer_[audioWriteIdx_] = pcm;
                audioWriteIdx_ = (audioWriteIdx_ + 1) % audioRingBuffer_.size();
                if (audioWriteIdx_ == audioReadIdx_) {
                    audioReadIdx_ = (audioReadIdx_ + 1) % audioRingBuffer_.size();
                }
            }

            {
                std::lock_guard<std::mutex> wLock(waveformMutex_);
                waveformBuffer_[waveformWriteIdx_] = pcm;
                waveformWriteIdx_ = (waveformWriteIdx_ + 1) % waveformBuffer_.size();
            }
        }
    }

    {
        std::lock_guard<std::mutex> sLock(statsMutex_);
        stats_.signalRssiDbfs = 10.0f * std::log10(std::max(1e-12f, powerAvg_));
        stats_.squelchOpen = (stats_.signalRssiDbfs >= cfg.squelchDbfs);
        stats_.tunedFreqMHz = (centerFreqHz + cfg.channelOffsetHz) / 1e6;
    }
}

size_t AmFmDemod::getAudioSamples(float* dest, size_t maxSamples) {
    std::lock_guard<std::mutex> lock(audioMutex_);
    size_t count = 0;
    while (count < maxSamples && audioReadIdx_ != audioWriteIdx_) {
        dest[count++] = audioRingBuffer_[audioReadIdx_];
        audioReadIdx_ = (audioReadIdx_ + 1) % audioRingBuffer_.size();
    }
    return count;
}

void AmFmDemod::getWaveform(std::vector<float>& outWaveform) const {
    std::lock_guard<std::mutex> lock(waveformMutex_);
    const size_t n = waveformBuffer_.size();
    outWaveform.resize(n);
    for (size_t i = 0; i < n; ++i) {
        outWaveform[i] = waveformBuffer_[(waveformWriteIdx_ + i) % n];
    }
}

AmFmStats AmFmDemod::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

} // namespace RTLSDR
