#!/usr/bin/env bash
set -e

echo "=== Building RTL-SDR TV & Spectrum Analyser for macOS ==="
mkdir -p build
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(sysctl -n hw.ncpu)

echo ""
echo "=== Build Succeeded! ==="
echo "Executable is at: ./build/RtlSdrTvSpectrum"
echo "To run:"
echo "  ./build/RtlSdrTvSpectrum"
