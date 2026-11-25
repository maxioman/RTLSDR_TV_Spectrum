#pragma once

#include <vector>
#include <complex>
#include <cstddef>

namespace RTLSDR {

enum class FftWindow {
    Rectangular = 0,
    Hann        = 1,
    Blackman    = 2,
    Hamming     = 3
};

class FastFft {
public:
    explicit FastFft(size_t size = 1024);
    ~FastFft() = default;

    void setSize(size_t size);
    size_t getSize() const { return size_; }

    void setWindow(FftWindow window);

    // Compute FFT from complex input to output dBFS (magnitude in dB)
    void process(const std::vector<std::complex<float>>& input, std::vector<float>& outDbfs);

    // Process from raw unsigned 8-bit I/Q bytes
    void processIqBytes(const uint8_t* iqData, size_t numPairs, std::vector<float>& outDbfs);

private:
    void precompute();
    void computeFft(std::vector<std::complex<float>>& a);

    size_t size_ = 1024;
    FftWindow windowType_ = FftWindow::Hann;

    std::vector<float> windowCoeffs_;
    std::vector<std::complex<float>> twiddle_;
    std::vector<size_t> bitReverse_;
    std::vector<std::complex<float>> workBuffer_;
};

} // namespace RTLSDR
