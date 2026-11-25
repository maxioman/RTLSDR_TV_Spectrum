#!/usr/bin/env bash
set -e

PI_HOST="192.168.1.27"
PI_USER="manohar"
TARGET_DIR="~/rtlsdr_tv_spectrum"

echo "=== Syncing source tree to Raspberry Pi 4 (${PI_USER}@${PI_HOST}) ==="

ssh -o ConnectTimeout=5 "${PI_USER}@${PI_HOST}" "mkdir -p ${TARGET_DIR}"

rsync -avz --exclude 'build*' --exclude '.git' ./ "${PI_USER}@${PI_HOST}:${TARGET_DIR}/"

echo ""
echo "=== Building natively on Raspberry Pi 4 ==="
ssh -t "${PI_USER}@${PI_HOST}" "
    cd ${TARGET_DIR}
    mkdir -p build && cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j4
    echo ''
    echo 'Build finished on Pi 4!'
    echo 'Run natively on Pi 4: ./build/RtlSdrTvSpectrum'
"
