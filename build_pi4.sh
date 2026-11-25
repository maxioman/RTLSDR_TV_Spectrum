#!/usr/bin/env bash
set -e

echo "=== Cross-Compiling RTL-SDR TV & Spectrum for Raspberry Pi 4 (aarch64) ==="

TOOLCHAIN_CMAKE="$(pwd)/cmake/aarch64-linux-gnu.cmake"

if ! command -v aarch64-unknown-linux-gnu-g++ &> /dev/null; then
    echo "Error: aarch64-unknown-linux-gnu-g++ not found in PATH."
    echo "Install it via: brew install messense/macos-cross-toolchains/aarch64-unknown-linux-gnu"
    exit 1
fi

mkdir -p build_pi4
cmake -B build_pi4 -S . \
    -DCMAKE_TOOLCHAIN_FILE="${TOOLCHAIN_CMAKE}" \
    -DCMAKE_BUILD_TYPE=Release

cmake --build build_pi4 -j$(sysctl -n hw.ncpu)

echo ""
echo "=== Pi 4 Cross-Compilation Complete! ==="
echo "Binary: ./build_pi4/RtlSdrTvSpectrum"
echo "To copy to your Raspberry Pi 4:"
echo "  scp ./build_pi4/RtlSdrTvSpectrum manohar@192.168.1.27:~/"
