#include "RtlTcpClient.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <chrono>
#include <iostream>

namespace RTLSDR {

static const char* getTunerName(uint32_t tunerType) {
    switch (tunerType) {
        case 1: return "Elonics E4000";
        case 2: return "Fitipower FC0012";
        case 3: return "Fitipower FC0013";
        case 4: return "FCI FC2580";
        case 5: return "Rafael Micro R820T";
        case 6: return "Rafael Micro R828D";
        default: return "Generic RTL2832U";
    }
}

RtlTcpClient::RtlTcpClient() = default;

RtlTcpClient::~RtlTcpClient() {
    close();
}

bool RtlTcpClient::open(const SdrConfig& config) {
    close();
    config_ = config;

    sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        std::cerr << "[RtlTcpClient] Failed to create socket\n";
        return false;
    }

    // Set socket receive buffer size to 2 MB to prevent buffer overflow drops
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(sockfd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in servAddr;
    std::memset(&servAddr, 0, sizeof(servAddr));
    servAddr.sin_family = AF_INET;
    servAddr.sin_port = htons(config.remotePort);

    struct hostent* server = gethostbyname(config.remoteHost.c_str());
    if (!server) {
        std::cerr << "[RtlTcpClient] Could not resolve host: " << config.remoteHost << "\n";
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }
    std::memcpy(&servAddr.sin_addr.s_addr, server->h_addr, server->h_length);

    // Connect with 3-second timeout
    int flags = fcntl(sockfd_, F_GETFL, 0);
    fcntl(sockfd_, F_SETFL, flags | O_NONBLOCK);

    int res = connect(sockfd_, (struct sockaddr*)&servAddr, sizeof(servAddr));
    if (res < 0 && errno == EINPROGRESS) {
        fd_set waitSet;
        FD_ZERO(&waitSet);
        FD_SET(sockfd_, &waitSet);
        struct timeval tv;
        tv.tv_sec = 3;
        tv.tv_usec = 0;
        if (select(sockfd_ + 1, nullptr, &waitSet, nullptr, &tv) > 0) {
            int so_error = 0;
            socklen_t len = sizeof(so_error);
            getsockopt(sockfd_, SOL_SOCKET, SO_ERROR, &so_error, &len);
            if (so_error != 0) {
                std::cerr << "[RtlTcpClient] Connect error: " << strerror(so_error) << "\n";
                ::close(sockfd_);
                sockfd_ = -1;
                return false;
            }
        } else {
            std::cerr << "[RtlTcpClient] Connection timed out to " << config.remoteHost << ":" << config.remotePort << "\n";
            ::close(sockfd_);
            sockfd_ = -1;
            return false;
        }
    } else if (res < 0) {
        std::cerr << "[RtlTcpClient] Connect failed: " << strerror(errno) << "\n";
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Return to blocking mode
    fcntl(sockfd_, F_SETFL, flags);

    // Read initial 12-byte handshake
    uint8_t handshake[12];
    ssize_t bytesRead = recv(sockfd_, handshake, 12, MSG_WAITALL);
    if (bytesRead == 12 && handshake[0] == 'R' && handshake[1] == 'T' && handshake[2] == 'L' && handshake[3] == '0') {
        uint32_t tunerType = (handshake[4] << 24) | (handshake[5] << 16) | (handshake[6] << 8) | handshake[7];
        uint32_t gainCount = (handshake[8] << 24) | (handshake[9] << 16) | (handshake[10] << 8) | handshake[11];

        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.connected = true;
        stats_.deviceName = std::string("rtl_tcp @ ") + config.remoteHost + ":" + std::to_string(config.remotePort);
        stats_.tunerType = getTunerName(tunerType);
        std::cout << "[RtlTcpClient] Handshake OK: " << stats_.tunerType << " (" << gainCount << " gain levels)\n";
    } else {
        std::cerr << "[RtlTcpClient] Invalid handshake response from rtl_tcp\n";
        ::close(sockfd_);
        sockfd_ = -1;
        return false;
    }

    // Send initial configuration commands
    setSampleRate(config_.sampleRateHz);
    setFrequency(config_.frequencyHz);
    if (config_.tunerAgc) {
        setTunerAgc(true);
    } else {
        setGain(config_.tunerGainTenthsDb);
    }
    setRtlAgc(config_.rtlAgc);
    setFrequencyCorrection(config_.frequencyCorrectionPpm);
    setDirectSampling(config_.directSampling);

    shouldStop_ = false;
    isRunning_ = true;
    workerThread_ = std::thread(&RtlTcpClient::networkThreadLoop, this);

    return true;
}

void RtlTcpClient::close() {
    shouldStop_ = true;
    if (sockfd_ >= 0) {
        shutdown(sockfd_, SHUT_RDWR);
        ::close(sockfd_);
        sockfd_ = -1;
    }
    if (workerThread_.joinable()) {
        workerThread_.join();
    }
    isRunning_ = false;

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.connected = false;
    stats_.currentKBytesPerSec = 0.0f;
}

bool RtlTcpClient::sendCommand(uint8_t cmd, uint32_t param) {
    if (sockfd_ < 0) return false;
    uint8_t packet[5];
    packet[0] = cmd;
    packet[1] = static_cast<uint8_t>((param >> 24) & 0xFF);
    packet[2] = static_cast<uint8_t>((param >> 16) & 0xFF);
    packet[3] = static_cast<uint8_t>((param >> 8) & 0xFF);
    packet[4] = static_cast<uint8_t>(param & 0xFF);
    return send(sockfd_, packet, 5, 0) == 5;
}

void RtlTcpClient::setFrequency(uint32_t frequencyHz) {
    config_.frequencyHz = frequencyHz;
    sendCommand(0x01, frequencyHz);
}

void RtlTcpClient::setSampleRate(uint32_t sampleRateHz) {
    config_.sampleRateHz = sampleRateHz;
    sendCommand(0x02, sampleRateHz);
}

void RtlTcpClient::setGain(int gainTenthsDb) {
    config_.tunerAgc = false;
    config_.tunerGainTenthsDb = gainTenthsDb;
    sendCommand(0x03, 1); // Manual gain mode
    sendCommand(0x04, static_cast<uint32_t>(gainTenthsDb));
}

void RtlTcpClient::setTunerAgc(bool enable) {
    config_.tunerAgc = enable;
    sendCommand(0x03, enable ? 0 : 1);
}

void RtlTcpClient::setRtlAgc(bool enable) {
    config_.rtlAgc = enable;
    sendCommand(0x08, enable ? 1 : 0);
}

void RtlTcpClient::setFrequencyCorrection(int ppm) {
    config_.frequencyCorrectionPpm = ppm;
    sendCommand(0x05, static_cast<uint32_t>(ppm));
}

void RtlTcpClient::setDirectSampling(int mode) {
    config_.directSampling = mode;
    sendCommand(0x09, static_cast<uint32_t>(mode));
}

SdrStats RtlTcpClient::getStats() const {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void RtlTcpClient::networkThreadLoop() {
    constexpr size_t CHUNK_SIZE = 64 * 1024;
    std::vector<uint8_t> buffer(CHUNK_SIZE);

    auto lastTime = std::chrono::steady_clock::now();
    uint64_t bytesSinceLastRate = 0;

    while (!shouldStop_ && sockfd_ >= 0) {
        ssize_t received = recv(sockfd_, buffer.data(), CHUNK_SIZE, 0);
        if (received > 0) {
            if (callback_) {
                callback_(buffer.data(), static_cast<size_t>(received));
            }

            bytesSinceLastRate += received;
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration<float>(now - lastTime).count();
            if (elapsed >= 0.5f) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.bytesReceived += bytesSinceLastRate;
                stats_.currentKBytesPerSec = (static_cast<float>(bytesSinceLastRate) / 1024.0f) / elapsed;
                bytesSinceLastRate = 0;
                lastTime = now;
            }
        } else if (received == 0) {
            std::cerr << "[RtlTcpClient] Server disconnected\n";
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                continue;
            }
            std::cerr << "[RtlTcpClient] recv error: " << strerror(errno) << "\n";
            break;
        }
    }

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.connected = false;
    stats_.currentKBytesPerSec = 0.0f;
    isRunning_ = false;
}

} // namespace RTLSDR
