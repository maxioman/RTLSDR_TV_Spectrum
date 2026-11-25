#include "Fft.hpp"
#include <cmath>
#include <algorithm>

namespace RTLSDR {

FastFft::FastFft(size_t size) {
    setSize(size);
}

void FastFft::setSize(size_t size) {
    // Round to next power of 2
    size_t p = 1;
    while (p < size) p <<= 1;
    size_ = p;

    precompute();
}

void FastFft::setWindow(FftWindow window) {
    windowType_ = window;
    precompute();
}

void FastFft::precompute() {
    windowCoeffs_.resize(size_);
    twiddle_.resize(size_ / 2);
    bitReverse_.resize(size_);
    workBuffer_.resize(size_);

    // 1. Window coefficients
    const double N = static_cast<double>(size_);
    for (size_t i = 0; i < size_; ++i) {
        double n = static_cast<double>(i);
        switch (windowType_) {
            case FftWindow::Hann:
                windowCoeffs_[i] = static_cast<float>(0.5 * (1.0 - std::cos(2.0 * M_PI * n / (N - 1.0))));
                break;
            case FftWindow::Hamming:
                windowCoeffs_[i] = static_cast<float>(0.54 - 0.46 * std::cos(2.0 * M_PI * n / (N - 1.0)));
                break;
            case FftWindow::Blackman:
                windowCoeffs_[i] = static_cast<float>(
                    0.42 - 0.5 * std::cos(2.0 * M_PI * n / (N - 1.0))
                         + 0.08 * std::cos(4.0 * M_PI * n / (N - 1.0))
                );
                break;
            case FftWindow::Rectangular:
            default:
                windowCoeffs_[i] = 1.0f;
                break;
        }
    }

    // 2. Twiddle factors
    for (size_t i = 0; i < size_ / 2; ++i) {
        double angle = -2.0 * M_PI * static_cast<double>(i) / N;
        twiddle_[i] = std::complex<float>(static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle)));
    }

    // 3. Bit-reversal permutation table
    size_t bits = 0;
    while ((1ULL << bits) < size_) bits++;

    for (size_t i = 0; i < size_; ++i) {
        size_t rev = 0;
        for (size_t b = 0; b < bits; ++b) {
            if ((i >> b) & 1) {
                rev |= (1ULL << (bits - 1 - b));
            }
        }
        bitReverse_[i] = rev;
    }
}

void FastFft::computeFft(std::vector<std::complex<float>>& a) {
    const size_t n = size_;

    // Bit-reversal reordering
    for (size_t i = 0; i < n; ++i) {
        size_t j = bitReverse_[i];
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }

    // Cooley-Tukey Radix-2 decimation-in-time
    for (size_t len = 2; len <= n; len <<= 1) {
        size_t half = len >> 1;
        size_t step = n / len;
        for (size_t i = 0; i < n; i += len) {
            for (size_t j = 0; j < half; ++j) {
                std::complex<float> u = a[i + j];
                std::complex<float> v = a[i + j + half] * twiddle_[j * step];
                a[i + j] = u + v;
                a[i + j + half] = u - v;
            }
        }
    }
}

void FastFft::process(const std::vector<std::complex<float>>& input, std::vector<float>& outDbfs) {
    size_t count = std::min(input.size(), size_);
    for (size_t i = 0; i < count; ++i) {
        workBuffer_[i] = input[i] * windowCoeffs_[i];
    }
    for (size_t i = count; i < size_; ++i) {
        workBuffer_[i] = std::complex<float>(0.0f, 0.0f);
    }

    computeFft(workBuffer_);

    // Output with FFT-shift (negative frequencies on left, positive frequencies on right)
    outDbfs.resize(size_);
    float norm = 1.0f / static_cast<float>(size_);
    size_t half = size_ / 2;

    for (size_t i = 0; i < size_; ++i) {
        size_t shiftedIdx = (i < half) ? (i + half) : (i - half);
        float mag = std::abs(workBuffer_[shiftedIdx]) * norm;
        float power = mag * mag;
        float dbfs = 10.0f * std::log10(std::max(1e-12f, power));
        outDbfs[i] = std::max(-120.0f, std::min(10.0f, dbfs));
    }
}

void FastFft::processIqBytes(const uint8_t* iqData, size_t numPairs, std::vector<float>& outDbfs) {
    size_t count = std::min(numPairs, size_);
    for (size_t i = 0; i < count; ++i) {
        float re = (static_cast<float>(iqData[i * 2 + 0]) - 127.5f) / 128.0f;
        float im = (static_cast<float>(iqData[i * 2 + 1]) - 127.5f) / 128.0f;
        workBuffer_[i] = std::complex<float>(re, im) * windowCoeffs_[i];
    }
    for (size_t i = count; i < size_; ++i) {
        workBuffer_[i] = std::complex<float>(0.0f, 0.0f);
    }

    computeFft(workBuffer_);

    // FFT-shift and convert to dBFS
    outDbfs.resize(size_);
    float norm = 1.0f / static_cast<float>(size_);
    size_t half = size_ / 2;

    for (size_t i = 0; i < size_; ++i) {
        size_t shiftedIdx = (i < half) ? (i + half) : (i - half);
        float mag = std::abs(workBuffer_[shiftedIdx]) * norm;
        float power = mag * mag;
        float dbfs = 10.0f * std::log10(std::max(1e-12f, power));
        outDbfs[i] = std::max(-120.0f, std::min(10.0f, dbfs));
    }
}

} // namespace RTLSDR
