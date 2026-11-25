cmake_minimum_required(VERSION 3.16)
project(RtlSdrTvSpectrum LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Source files
set(SOURCES
    src/main.cpp
    src/sdr/RtlTcpClient.cpp
    src/sdr/RtlUsbDevice.cpp
    src/sdr/TestSignalGenerator.cpp
    src/dsp/Fft.cpp
    src/dsp/SpectrumEngine.cpp
    src/dsp/SpectrumSweepEngine.cpp
    src/dsp/AnalogTvDemod.cpp
    src/dsp/AmFmDemod.cpp
    src/audio/AudioOutput.cpp
    src/ui/MainWindow.cpp

    # Dear ImGui Core
    vendor/imgui/imgui.cpp
    vendor/imgui/imgui_draw.cpp
    vendor/imgui/imgui_tables.cpp
    vendor/imgui/imgui_widgets.cpp
    vendor/imgui/backends/imgui_impl_glfw.cpp
    vendor/imgui/backends/imgui_impl_opengl3.cpp

    # ImPlot
    vendor/implot/implot.cpp
    vendor/implot/implot_items.cpp
)

add_executable(RtlSdrTvSpectrum ${SOURCES})

target_include_directories(RtlSdrTvSpectrum PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/imgui/backends
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/implot
    ${CMAKE_CURRENT_SOURCE_DIR}/vendor/stb
)

find_package(Threads REQUIRED)
target_link_libraries(RtlSdrTvSpectrum PRIVATE Threads::Threads)

# Check for librtlsdr
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(RTLSDR librtlsdr)
    if(RTLSDR_FOUND)
        target_include_directories(RtlSdrTvSpectrum PRIVATE ${RTLSDR_INCLUDE_DIRS})
        target_link_directories(RtlSdrTvSpectrum PRIVATE ${RTLSDR_LIBRARY_DIRS} /opt/homebrew/lib)
        target_link_libraries(RtlSdrTvSpectrum PRIVATE ${RTLSDR_LIBRARIES})
        target_compile_definitions(RtlSdrTvSpectrum PRIVATE HAVE_LIBRTLSDR=1)
        message(STATUS "Found librtlsdr: enabling direct USB dongle support")
    else()
        message(STATUS "librtlsdr not found: rtl_tcp network client and generator enabled")
    endif()
endif()

# Platform specific configurations
if(APPLE)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 REQUIRED)

    target_link_libraries(RtlSdrTvSpectrum PRIVATE
        glfw
        OpenGL::GL
        "-framework Cocoa"
        "-framework IOKit"
        "-framework CoreVideo"
        "-framework AudioToolbox"
    )
elseif(UNIX AND NOT APPLE)
    # Linux (Raspberry Pi 4 / Debian)
    find_package(OpenGL REQUIRED)
    find_package(glfw3 QUIET)

    if(glfw3_FOUND)
        target_link_libraries(RtlSdrTvSpectrum PRIVATE glfw)
    else()
        target_link_libraries(RtlSdrTvSpectrum PRIVATE glfw GL dl m)
    endif()

    target_link_libraries(RtlSdrTvSpectrum PRIVATE OpenGL::GL m dl)
endif()
