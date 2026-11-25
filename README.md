# RTL-SDR Analog TV Viewer & VHF/UHF Spectrum Analyser

A cross-platform C++17 application for **macOS** and **Raspberry Pi 4** (`aarch64`) that combines a real-time **Analog TV demodulator** with a **VHF/UHF Spectrum Analyser & 2D Waterfall**.

---

## Features

### 1. Analog TV Viewer
- **Real-time AM-VSB Demodulation**: Decodes live 8-bit I/Q streams into a 720-pixel-wide raster (`720x576` for 625-line standards, `720x480` for 525-line NTSC).
- **Flywheel PLL Sync & Deinterlacing**: Automatic horizontal and vertical sync lock with manual phase/fine-frequency trimmers and progressive deinterlacing.
- **Video Line IRE Scope**: Real-time oscilloscope view of the active scanline waveform with sync and black-level markers.
- **Supported TV Standards**:
  - **PAL B/G**: 625 lines / 50 Hz, 15,625 Hz line rate, +5.5 MHz FM sound (negative AM video)
  - **PAL I**: 625 lines / 50 Hz, 15,625 Hz line rate, +6.0 MHz FM sound
  - **PAL D/K**: 625 lines / 50 Hz, 15,625 Hz line rate, +6.5 MHz FM sound
  - **NTSC M**: 525 lines / 60 Hz, 15,734.26 Hz line rate, +4.5 MHz FM sound
  - **SECAM L**: 625 lines / 50 Hz, 15,625 Hz line rate, +6.5 MHz AM sound (positive AM video)
- **Channel Presets**: VHF Band I (E2–E4), VHF Band III (E5–E12), Cable S-Channels (S01–S41), and UHF TV Grid (CH 21–69).

### 2. VHF / UHF Spectrum Analyser & 2D Waterfall
- **Real-time Radix-2 FFT**: Configurable FFT power spectrum with Max Hold, exponential averaging, peak tracking, and noise-floor readout.
- **2D Waterfall Spectrogram**: Scrolling GPU-textured spectrogram with **Turbo**, **Viridis**, **Plasma**, and **Fire** color palettes.

### 3. AM / FM Wideband Spectrum Sweep & Demodulator
- **Stepped Wideband Sweep**: Scans across any configured `[Start, Stop]` MHz span, stitching FFT segments into a unified spectrum trace and 2D waterfall with Max Hold and click-to-tune.
- **Band Presets**: FM Broadcast (88–108 MHz), Airband AM (118–137 MHz), 2m Ham NFM (144–148 MHz), and 70cm / ISM (430–440 MHz).
- **Real-Time AM / NFM / WFM Demodulation**: Cascaded I/Q channel filtering, quadrature FM discriminator (with selectable 50 µs / 75 µs de-emphasis) and envelope AM demodulator with AGC, squelch, live RSSI meter, AF audio oscilloscope, and 48 kHz cross-platform audio playback (`AudioToolbox` on macOS, `ALSA` on Linux).

### 4. Flexible SDR Input Sources
- **Network Streaming (`rtl_tcp`)**: Connects over TCP to a remote Raspberry Pi or host running `rtl_tcp`.
- **Local USB RTL-SDR**: Direct `librtlsdr` support when compiled with `librtlsdr` installed.
- **Synthetic Test Pattern Generator**: Built-in RF I/Q generator with **PM5544 Testcard**, **EBU Colorbars**, and **Custom BMP Image** (`test_pattern.bmp`) modes, plus simulated WFM/AM/NFM stations in the FM/Airband/Ham/ISM bands for offline testing.

---

## Architecture

```
                     ┌────────────────────────┐
                     │     RTL-SDR Dongle     │
                     └───────────┬────────────┘
                                 │ USB
                   ┌─────────────▼────────────┐
                   │   Raspberry Pi 4 (SDR)   │
                   │    rtl_tcp -a 0.0.0.0    │
                   └─────────────┬────────────┘
                                 │ TCP (Port 1234)
         ┌───────────────────────┴───────────────────────┐
         ▼                                               ▼
┌───────────────────────────┐               ┌───────────────────────────┐
│     macOS Desktop App     │               │     Native Pi 4 Display   │
│  (Remote Client / Viewer) │               │      (Local HDMI / GL)    │
├───────────────────────────┤               ├───────────────────────────┤
│ • Analog TV CRT Display   │               │ • Analog TV CRT Display   │
│ • VHF/UHF Spectrum Graph  │               │ • VHF/UHF Spectrum Graph  │
│ • 2D Waterfall Spectrogram│               │ • 2D Waterfall Spectrogram│
│ • Video Line IRE Scope    │               │ • Video Line IRE Scope    │
└───────────────────────────┘               └───────────────────────────┘
```

---

## Prerequisites

### macOS
- CMake >= 3.16 and a C++17 compiler (Xcode Command Line Tools)
- `glfw` and `pkg-config` (via Homebrew):
  ```bash
  brew install cmake glfw pkg-config
  ```
- Optional (for direct USB RTL-SDR support):
  ```bash
  brew install librtlsdr
  ```

### Raspberry Pi 4 / Debian (`aarch64`)
```bash
sudo apt update
sudo apt install -y build-essential cmake pkg-config libglfw3-dev libgl-dev librtlsdr-dev rtl-sdr
```

---

## Build & Run

### macOS
```bash
./build_macos.sh
./build/RtlSdrTvSpectrum
```

On launch, the app defaults to the internal **Test Pattern Generator** (or you can switch to `rtl_tcp` / `Local USB` from the top bar).

### Remote Streaming from Raspberry Pi 4 (`rtl_tcp`)
1. On the Raspberry Pi 4, start `rtl_tcp`:
   ```bash
   rtl_tcp -a 0.0.0.0 -p 1234
   ```
2. In the app:
   - Select SDR Source: `rtl_tcp Network`
   - Enter your Pi's IP address (default `192.168.1.27`) and port `1234`
   - Click **CONNECT**

### Cross-Compiling for Raspberry Pi 4 (from macOS)
Requires the `aarch64-unknown-linux-gnu` toolchain (`brew install messense/macos-cross-toolchains/aarch64-unknown-linux-gnu`):
```bash
./build_pi4.sh
```
The compiled binary is written to `build_pi4/RtlSdrTvSpectrum`.

To sync the source tree and build natively on a Raspberry Pi 4 over SSH, update `PI_HOST` / `PI_USER` in `deploy_to_pi4.sh` and run:
```bash
./deploy_to_pi4.sh
```

---

## Project Structure

```
├── CMakeLists.txt
├── build_macos.sh               # macOS Release build script
├── build_pi4.sh                 # aarch64 cross-compilation script
├── deploy_to_pi4.sh             # rsync + native build helper for Pi 4
├── cmake/
│   └── aarch64-linux-gnu.cmake  # CMake toolchain file for Pi 4
├── src/
│   ├── main.cpp                 # GLFW / OpenGL3 / Dear ImGui entry point
│   ├── sdr/                     # ISdrDevice, RtlTcpClient, RtlUsbDevice, TestSignalGenerator
│   ├── dsp/                     # FastFft, SpectrumEngine, SpectrumSweepEngine, AnalogTvDemod, AmFmDemod
│   ├── audio/                   # Cross-platform 48 kHz AudioOutput (AudioToolbox / ALSA)
│   └── ui/                      # MainWindow (TV viewer, spectrum, AM/FM sweep & demod, diagnostics)
├── vendor/
│   ├── imgui/                   # Dear ImGui
│   ├── implot/                  # ImPlot
│   └── stb/                     # stb_image.h
└── test_pattern.bmp             # Sample image for custom RF test pattern mode
```

---

## Third-Party Libraries

- [Dear ImGui](https://github.com/ocornut/imgui)
- [ImPlot](https://github.com/epezent/implot)
- [stb_image](https://github.com/nothings/stb)
