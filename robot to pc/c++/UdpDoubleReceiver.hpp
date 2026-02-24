#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

class UdpDoubleReceiver {
public:
    enum class Endian {
        Big,
        Little
    };

    struct Packet {
        std::uint32_t seq = 0;
        std::uint64_t timestampNanos = 0;
        std::vector<double> data;
    };

    // host: "0.0.0.0" recommended (bind all interfaces)
    UdpDoubleReceiver(const std::string& host,
                      int port,
                      std::size_t bufferSize = 2048,
                      Endian endian = Endian::Big);

    ~UdpDoubleReceiver();

    bool start();
    void stop();

    // Copies latest packet into out. Returns false if nothing received yet.
    bool getLatest(Packet& out);

    bool isRunning() const { return running_.load(); }

private:
    void run();

    // Endian-aware readers
    static std::uint16_t read16(const std::uint8_t* p, Endian e);
    static std::uint32_t read32(const std::uint8_t* p, Endian e);
    static std::uint64_t read64(const std::uint8_t* p, Endian e);
    static double readDouble(const std::uint8_t* p, Endian e);

private:
    std::string host_;
    int port_;
    std::size_t bufferSize_;
    Endian endian_;

    std::atomic<bool> running_;
    std::thread receiverThread_;

    SOCKET sockfd_;
    bool wsaInitialized_ = false;

    std::vector<std::uint8_t> recvBuffer_;

    std::mutex dataMutex_;
    Packet latest_;
    bool hasData_ = false;
};
