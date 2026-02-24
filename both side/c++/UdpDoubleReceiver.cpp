
#ifndef _WIN32_WINNT
  #define _WIN32_WINNT 0x0600   // Vista+ (enables inet_pton / InetPton)
#endif

#ifdef _MSC_VER
#pragma comment(lib,"Ws2_32.lib")
#endif

#include "UdpDoubleReceiver.hpp"
#include <iostream>
#include <cstring>
#include <chrono>
#include <ws2tcpip.h>

static constexpr std::uint32_t MAGIC_UDPD = 0x55445044; // 'U''D''P''D'
static constexpr std::uint16_t VERSION_1  = 1;
static constexpr std::size_t   HEADER_BYTES = 20;

UdpDoubleReceiver::UdpDoubleReceiver(const std::string& host,
                                     int port,
                                     std::size_t bufferSize,
                                     Endian endian)
    : host_(host),
      port_(port),
      bufferSize_(bufferSize),
      endian_(endian),
      running_(false),
      sockfd_(INVALID_SOCKET)
{
    if (bufferSize_ < 256) bufferSize_ = 256; // small safety minimum
    recvBuffer_.resize(bufferSize_);
}

UdpDoubleReceiver::~UdpDoubleReceiver() {
    stop();
}

bool UdpDoubleReceiver::start() {
    if (running_) return true;

    // WinSock init
    WSADATA wsaData;
    int wsaOk = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaOk != 0) {
        std::cerr << "WSAStartup failed, error: " << wsaOk << "\n";
        return false;
    }
    wsaInitialized_ = true;

    // Create socket
    sockfd_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd_ == INVALID_SOCKET) {
        std::cerr << "socket() failed: " << WSAGetLastError() << "\n";
        // keep WSACleanup for stop()
        return false;
    }

    // Allow quick restart
    BOOL reuse = TRUE;
    setsockopt(sockfd_, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    // Non-blocking
    u_long mode = 1;
    if (ioctlsocket(sockfd_, FIONBIO, &mode) != 0) {
        std::cerr << "ioctlsocket(FIONBIO) failed: " << WSAGetLastError() << "\n";
        // not fatal, but strongly preferred
    }

    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(static_cast<u_short>(port_));

    if (host_ == "0.0.0.0") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else {
        int ok = inet_pton(AF_INET, host_.c_str(), &addr.sin_addr);
        if (ok != 1) {
            std::cerr << "inet_pton failed for host=" << host_ << "\n";
            return false;
        }
    }

    if (bind(sockfd_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed: " << WSAGetLastError() << "\n";
        return false;
    }

    running_ = true;
    receiverThread_ = std::thread(&UdpDoubleReceiver::run, this);

    std::cout << "UDP receiver listening on " << host_ << ":" << port_
              << " (endian=" << (endian_ == Endian::Big ? "BIG" : "LITTLE") << ")\n";
    return true;
}

void UdpDoubleReceiver::stop() {
    if (!running_) {
        // still cleanup if half-started
        if (sockfd_ != INVALID_SOCKET) {
            closesocket(sockfd_);
            sockfd_ = INVALID_SOCKET;
        }
        if (wsaInitialized_) {
            WSACleanup();
            wsaInitialized_ = false;
        }
        return;
    }

    running_ = false;

    if (sockfd_ != INVALID_SOCKET) {
        closesocket(sockfd_);
        sockfd_ = INVALID_SOCKET;
    }

    if (receiverThread_.joinable())
        receiverThread_.join();

    if (wsaInitialized_) {
        WSACleanup();
        wsaInitialized_ = false;
    }
}

bool UdpDoubleReceiver::getLatest(Packet& out) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    if (!hasData_) return false;
    out = latest_;   // copy out (safe & simple)
    return true;
}

std::uint16_t UdpDoubleReceiver::read16(const std::uint8_t* p, Endian e) {
    if (e == Endian::Big) {
        return (std::uint16_t(p[0]) << 8) | std::uint16_t(p[1]);
    } else {
        return (std::uint16_t(p[1]) << 8) | std::uint16_t(p[0]);
    }
}

std::uint32_t UdpDoubleReceiver::read32(const std::uint8_t* p, Endian e) {
    if (e == Endian::Big) {
        return (std::uint32_t(p[0]) << 24) |
               (std::uint32_t(p[1]) << 16) |
               (std::uint32_t(p[2]) << 8)  |
               (std::uint32_t(p[3]));
    } else {
        return (std::uint32_t(p[3]) << 24) |
               (std::uint32_t(p[2]) << 16) |
               (std::uint32_t(p[1]) << 8)  |
               (std::uint32_t(p[0]));
    }
}

std::uint64_t UdpDoubleReceiver::read64(const std::uint8_t* p, Endian e) {
    std::uint64_t v = 0;
    if (e == Endian::Big) {
        for (int i = 0; i < 8; ++i) v = (v << 8) | std::uint64_t(p[i]);
    } else {
        for (int i = 7; i >= 0; --i) v = (v << 8) | std::uint64_t(p[i]);
    }
    return v;
}

double UdpDoubleReceiver::readDouble(const std::uint8_t* p, Endian e) {
    std::uint64_t bits = read64(p, e);
    double d;
    std::memcpy(&d, &bits, sizeof(d));
    return d;
}

void UdpDoubleReceiver::run() {
    while (running_) {
        sockaddr_in from{};
        int fromLen = sizeof(from);

        int received = recvfrom(
            sockfd_,
            (char*)recvBuffer_.data(),
            (int)recvBuffer_.size(),
            0,
            (sockaddr*)&from,
            &fromLen
        );

        if (!running_) break;

        if (received == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAEWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            std::cerr << "recvfrom() error: " << err << "\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (received < (int)HEADER_BYTES) {
            continue; // too small
        }

        const std::uint8_t* p = recvBuffer_.data();

        // Header is always in the sender-selected endian too.
        std::uint32_t magic = read32(p + 0, endian_);
        std::uint16_t ver   = read16(p + 4, endian_);
        std::uint16_t count = read16(p + 6, endian_);
        std::uint32_t seq   = read32(p + 8, endian_);
        std::uint64_t ts    = read64(p + 12, endian_);

        if (magic != MAGIC_UDPD) continue;
        if (ver != VERSION_1) continue;

        std::size_t expectedBytes = HEADER_BYTES + (std::size_t(count) * 8);
        if (expectedBytes > (std::size_t)received) {
            continue; // truncated packet
        }

        Packet pkt;
        pkt.seq = seq;
        pkt.timestampNanos = ts;
        pkt.data.resize(count);

        const std::uint8_t* dptr = p + HEADER_BYTES;
        for (std::size_t i = 0; i < count; ++i) {
            pkt.data[i] = readDouble(dptr + i * 8, endian_);
        }

        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            latest_ = std::move(pkt);
            hasData_ = true;
        }
    }
}
