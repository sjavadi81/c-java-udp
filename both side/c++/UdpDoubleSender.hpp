#pragma once

#include <cstdint>
#include <string>
#include <vector>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif



class UdpDoubleSender {
public:
    enum class IpMode { Any, IPv4, IPv6 };

    // Interop constants (match Java)
    static constexpr uint32_t MAGIC = 0x55445044u; // "UDPD"
    static constexpr uint16_t VERSION = 1;
    static constexpr int HEADER_BYTES = 20;
    static constexpr int DEFAULT_MAX_UDP_PAYLOAD = 1400;

    UdpDoubleSender(const std::string& remoteHost,
                     uint16_t remotePort,
                     uint16_t localPort,
                     int maxDoubles,
                     bool connectUdp = true,
                     int maxPayloadBytes = DEFAULT_MAX_UDP_PAYLOAD,
                     IpMode ipMode = IpMode::Any,
                     int requestedSndBuf = 128 * 1024);

    // Move-only
    UdpDoubleSender(const UdpDoubleSender&) = delete;
    UdpDoubleSender& operator=(const UdpDoubleSender&) = delete;

    UdpDoubleSender(UdpDoubleSender&& other) noexcept;
    UdpDoubleSender& operator=(UdpDoubleSender&& other) noexcept;

    ~UdpDoubleSender();

    int  getMaxDoubles() const;
    int  getSendBufferBytes() const;

    // TTL / hop limit
    void setUnicastHopLimit(int hops);
    void setMulticastHopLimit(int hops);

    // Multicast behavior
    void setMulticastLoop(bool enable);
    void setMulticastInterfaceIPv4(uint32_t ifAddrNbo);
    void setMulticastInterfaceIPv6(unsigned int ifIndex);

    // Sending
    size_t sendAutoSeq(const double* data, int count);
    size_t sendWithSeq(const double* data, int count, int32_t seq, int64_t timestampNanos = INT64_MIN);

    void close();

private:
#if defined(_WIN32)
    SOCKET sock_ = INVALID_SOCKET;
    struct WinsockRAII;
    WinsockRAII* winsock_ = nullptr;
#else
    int sock_ = -1;
#endif

    std::string remoteHost_;
    uint16_t remotePort_ = 0;
    bool connect_ = false;
    IpMode ipMode_ = IpMode::Any;

    int family_ = 0;
    sockaddr_storage destAddr_{};
    socklen_t destAddrLen_ = 0;

    int maxDoubles_ = 0;
    int32_t seq_ = 0;

    std::vector<uint8_t> buffer_;

    int64_t tsOffset_ = 0; // to avoid negative steady_clock nanos

private:
    bool   isOpen_() const;
    void   closeSock_();
    void   moveFrom_(UdpDoubleSender&& o) noexcept;

    void   initTimestampOffset_();
    int64_t monotonicNowNanosNonNegative_() const;

    static int desiredFamily_(IpMode m);
    static std::string lastSockErr_();

    void   createAndConfigureSocket_(uint16_t localPort, int requestedSndBuf);
    static bool bindLocal_(decltype(sock_) s, int family, uint16_t localPort);

    // endian / packing helpers
    static bool     isLittleEndian_();
    static uint64_t bswap64_(uint64_t x);
    static uint64_t hostToBE64_(uint64_t v);

    static void writeBE16_(uint8_t* dst, uint16_t v);
    static void writeBE32u_(uint8_t* dst, uint32_t u);
    static void writeBE32_fromBits_(uint8_t* dst, int32_t s);
    static void writeBE64_fromBits_(uint8_t* dst, int64_t s);
    static void writeDoubleBE_(uint8_t* dst, double d);
};