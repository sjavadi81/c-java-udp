#include "UdpDoubleSender.hpp"

#include <cstring>
#include <stdexcept>
#include <algorithm>
#include <chrono>

#if !defined(_WIN32)
  #include <arpa/inet.h>
  #include <errno.h>
  #include <netdb.h>
  #include <unistd.h>
  #include <netinet/in.h>
#endif

#ifdef _MSC_VER
  #pragma comment(lib, "ws2_32.lib")
#endif

// ---------- platform error string ----------
std::string UdpDoubleSender::lastSockErr_() {
#if defined(_WIN32)
    return "WSAGetLastError=" + std::to_string(WSAGetLastError());
#else
    int e = errno;
    return "errno=" + std::to_string(e) + " (" + std::string(std::strerror(e)) + ")";
#endif
}

#if defined(_WIN32)
struct UdpDoubleSender::WinsockRAII {
    WinsockRAII() {
        WSADATA w{};
        if (WSAStartup(MAKEWORD(2,2), &w) != 0) {
            throw std::runtime_error("WSAStartup failed: " + UdpDoubleSender::lastSockErr_());
        }
    }
    ~WinsockRAII() { WSACleanup(); }
};
#endif

// ---------- endian helpers ----------
bool UdpDoubleSender::isLittleEndian_() {
    uint16_t one = 1;
    return *reinterpret_cast<uint8_t*>(&one) == 1;
}

uint64_t UdpDoubleSender::bswap64_(uint64_t x) {
#if defined(_MSC_VER)
    return _byteswap_uint64(x);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(x);
#else
    return ((x & 0x00000000000000FFULL) << 56) |
           ((x & 0x000000000000FF00ULL) << 40) |
           ((x & 0x0000000000FF0000ULL) << 24) |
           ((x & 0x00000000FF000000ULL) << 8)  |
           ((x & 0x000000FF00000000ULL) >> 8)  |
           ((x & 0x0000FF0000000000ULL) >> 24) |
           ((x & 0x00FF000000000000ULL) >> 40) |
           ((x & 0xFF00000000000000ULL) >> 56);
#endif
}

uint64_t UdpDoubleSender::hostToBE64_(uint64_t v) {
    return isLittleEndian_() ? bswap64_(v) : v;
}

void UdpDoubleSender::writeBE16_(uint8_t* dst, uint16_t v) {
    uint16_t be = htons(v);
    std::memcpy(dst, &be, sizeof(be));
}

void UdpDoubleSender::writeBE32u_(uint8_t* dst, uint32_t u) {
    uint32_t be = htonl(u);
    std::memcpy(dst, &be, sizeof(be));
}

void UdpDoubleSender::writeBE32_fromBits_(uint8_t* dst, int32_t s) {
    uint32_t u = 0;
    static_assert(sizeof(u) == sizeof(s), "width mismatch");
    std::memcpy(&u, &s, sizeof(u));   // preserve bits
    writeBE32u_(dst, u);
}

void UdpDoubleSender::writeBE64_fromBits_(uint8_t* dst, int64_t s) {
    uint64_t u = 0;
    static_assert(sizeof(u) == sizeof(s), "width mismatch");
    std::memcpy(&u, &s, sizeof(u));   // preserve bits
    uint64_t be = hostToBE64_(u);
    std::memcpy(dst, &be, sizeof(be));
}

void UdpDoubleSender::writeDoubleBE_(uint8_t* dst, double d) {
    static_assert(sizeof(double) == 8, "double must be 8 bytes");
    uint64_t bits = 0;
    std::memcpy(&bits, &d, sizeof(bits));
    bits = hostToBE64_(bits);
    std::memcpy(dst, &bits, sizeof(bits));
}

// ---------- misc ----------
int UdpDoubleSender::desiredFamily_(IpMode m) {
    switch (m) {
        case IpMode::IPv4: return AF_INET;
        case IpMode::IPv6: return AF_INET6;
        default: return AF_UNSPEC;
    }
}

bool UdpDoubleSender::isOpen_() const {
#if defined(_WIN32)
    return sock_ != INVALID_SOCKET;
#else
    return sock_ >= 0;
#endif
}

void UdpDoubleSender::closeSock_() {
#if defined(_WIN32)
    if (sock_ != INVALID_SOCKET) { closesocket(sock_); sock_ = INVALID_SOCKET; }
#else
    if (sock_ >= 0) { ::close(sock_); sock_ = -1; }
#endif
    family_ = 0;
    destAddrLen_ = 0;
    std::memset(&destAddr_, 0, sizeof(destAddr_));
}

void UdpDoubleSender::initTimestampOffset_() {
    using namespace std::chrono;
    const int64_t now = (int64_t)duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
    tsOffset_ = (now < 0) ? -now : 0;
}

int64_t UdpDoubleSender::monotonicNowNanosNonNegative_() const {
    using namespace std::chrono;
    const int64_t now = (int64_t)duration_cast<nanoseconds>(
        steady_clock::now().time_since_epoch()).count();
    return now + tsOffset_;
}

// ---------- ctor/dtor ----------
UdpDoubleSender::UdpDoubleSender(const std::string& remoteHost,
                                   uint16_t remotePort,
                                   uint16_t localPort,
                                   int maxDoubles,
                                   bool connectUdp,
                                   int maxPayloadBytes,
                                   IpMode ipMode,
                                   int requestedSndBuf)
: remoteHost_(remoteHost),
  remotePort_(remotePort),
  connect_(connectUdp),
  ipMode_(ipMode)
{
#if defined(_WIN32)
    winsock_ = new WinsockRAII();
#endif

    const int payloadLimit = (maxPayloadBytes > 0) ? maxPayloadBytes : DEFAULT_MAX_UDP_PAYLOAD;
    const int maxByPayload = std::max(0, (payloadLimit - HEADER_BYTES) / 8);
    maxDoubles_ = std::max(0, std::min(maxDoubles, maxByPayload));

    buffer_.resize((size_t)HEADER_BYTES + (size_t)maxDoubles_ * 8);

    initTimestampOffset_();
    createAndConfigureSocket_(localPort, requestedSndBuf);
}

UdpDoubleSender::~UdpDoubleSender() {
    close();
#if defined(_WIN32)
    delete winsock_;
    winsock_ = nullptr;
#endif
}

UdpDoubleSender::UdpDoubleSender(UdpDoubleSender&& other) noexcept { moveFrom_(std::move(other)); }

UdpDoubleSender& UdpDoubleSender::operator=(UdpDoubleSender&& other) noexcept {
    if (this != &other) {
        close();
#if defined(_WIN32)
        delete winsock_;
        winsock_ = other.winsock_;
        other.winsock_ = nullptr;
#endif
        moveFrom_(std::move(other));
    }
    return *this;
}

void UdpDoubleSender::moveFrom_(UdpDoubleSender&& o) noexcept {
    remoteHost_ = std::move(o.remoteHost_);
    remotePort_ = o.remotePort_;
    connect_ = o.connect_;
    ipMode_ = o.ipMode_;

    family_ = o.family_;
    destAddr_ = o.destAddr_;
    destAddrLen_ = o.destAddrLen_;

    maxDoubles_ = o.maxDoubles_;
    seq_ = o.seq_;
    buffer_ = std::move(o.buffer_);

    tsOffset_ = o.tsOffset_;

#if defined(_WIN32)
    sock_ = o.sock_;
    o.sock_ = INVALID_SOCKET;
#else
    sock_ = o.sock_;
    o.sock_ = -1;
#endif

    o.family_ = 0;
    o.destAddrLen_ = 0;
    std::memset(&o.destAddr_, 0, sizeof(o.destAddr_));
}

// ---------- socket setup ----------
bool UdpDoubleSender::bindLocal_(decltype(sock_) s, int family, uint16_t localPort) {
    if (localPort == 0) return true;

    if (family == AF_INET) {
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons(localPort);
        return ::bind(s, (sockaddr*)&a, (socklen_t)sizeof(a)) == 0;
    }
    if (family == AF_INET6) {
        sockaddr_in6 a{};
        a.sin6_family = AF_INET6;
        a.sin6_addr = in6addr_any;
        a.sin6_port = htons(localPort);
        return ::bind(s, (sockaddr*)&a, (socklen_t)sizeof(a)) == 0;
    }
    return false;
}

void UdpDoubleSender::createAndConfigureSocket_(uint16_t localPort, int requestedSndBuf) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_family   = desiredFamily_(ipMode_);

    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(remotePort_);

    int rc = getaddrinfo(remoteHost_.c_str(), portStr.c_str(), &hints, &res);
    if (rc != 0 || !res) {
#if defined(_WIN32)
        throw std::runtime_error("getaddrinfo failed: rc=" + std::to_string(rc));
#else
        throw std::runtime_error(std::string("getaddrinfo failed: ") + gai_strerror(rc));
#endif
    }

    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
#if defined(_WIN32)
        SOCKET s = ::socket((int)ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (s == INVALID_SOCKET) continue;
#else
        int s = ::socket((int)ai->ai_family, SOCK_DGRAM, IPPROTO_UDP);
        if (s < 0) continue;
#endif

        if (requestedSndBuf > 0) {
            setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                       (const char*)&requestedSndBuf, (socklen_t)sizeof(requestedSndBuf));
        }

        if (!bindLocal_(s, (int)ai->ai_family, localPort)) {
#if defined(_WIN32)
            closesocket(s);
#else
            ::close(s);
#endif
            continue;
        }

        std::memset(&destAddr_, 0, sizeof(destAddr_));
        std::memcpy(&destAddr_, ai->ai_addr, (size_t)ai->ai_addrlen);
        destAddrLen_ = (socklen_t)ai->ai_addrlen;
        family_ = (int)ai->ai_family;

        if (connect_) {
            if (::connect(s, (sockaddr*)&destAddr_, destAddrLen_) != 0) {
#if defined(_WIN32)
                closesocket(s);
#else
                ::close(s);
#endif
                continue;
            }
        }

        sock_ = s;
        freeaddrinfo(res);
        return;
    }

    freeaddrinfo(res);
    throw std::runtime_error("Failed to create usable UDP socket for any resolved address");
}

// ---------- public API ----------
int UdpDoubleSender::getMaxDoubles() const { return maxDoubles_; }

int UdpDoubleSender::getSendBufferBytes() const {
    if (!isOpen_()) return 0;
    int v = 0; socklen_t len = (socklen_t)sizeof(v);
    if (getsockopt(sock_, SOL_SOCKET, SO_SNDBUF, (char*)&v, &len) != 0) return 0;
    return v;
}

void UdpDoubleSender::setUnicastHopLimit(int hops) {
    if (!isOpen_()) return;
    hops = std::max(0, std::min(hops, 255));
    if (family_ == AF_INET) {
        setsockopt(sock_, IPPROTO_IP, IP_TTL, (char*)&hops, (socklen_t)sizeof(hops));
    } else if (family_ == AF_INET6) {
        setsockopt(sock_, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (char*)&hops, (socklen_t)sizeof(hops));
    }
}

void UdpDoubleSender::setMulticastHopLimit(int hops) {
    if (!isOpen_()) return;
    hops = std::max(0, std::min(hops, 255));
    if (family_ == AF_INET) {
        unsigned char ttl = (unsigned char)hops;
        setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_TTL, (char*)&ttl, (socklen_t)sizeof(ttl));
    } else if (family_ == AF_INET6) {
        int h = hops;
        setsockopt(sock_, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (char*)&h, (socklen_t)sizeof(h));
    }
}

void UdpDoubleSender::setMulticastLoop(bool enable) {
    if (!isOpen_()) return;
    if (family_ == AF_INET) {
        unsigned char v = enable ? 1 : 0;
        setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_LOOP, (char*)&v, (socklen_t)sizeof(v));
    } else if (family_ == AF_INET6) {
        unsigned int v = enable ? 1u : 0u;
        setsockopt(sock_, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, (char*)&v, (socklen_t)sizeof(v));
    }
}

void UdpDoubleSender::setMulticastInterfaceIPv4(uint32_t ifAddrNbo) {
    if (!isOpen_() || family_ != AF_INET) return;
    in_addr a{}; a.s_addr = ifAddrNbo;
    setsockopt(sock_, IPPROTO_IP, IP_MULTICAST_IF, (char*)&a, (socklen_t)sizeof(a));
}

void UdpDoubleSender::setMulticastInterfaceIPv6(unsigned int ifIndex) {
    if (!isOpen_() || family_ != AF_INET6) return;
    setsockopt(sock_, IPPROTO_IPV6, IPV6_MULTICAST_IF, (char*)&ifIndex, (socklen_t)sizeof(ifIndex));
}

size_t UdpDoubleSender::sendAutoSeq(const double* data, int count) {
    return sendWithSeq(data, count, seq_++);
}

size_t UdpDoubleSender::sendWithSeq(const double* data, int count, int32_t seq, int64_t timestampNanos) {
    if (!data) throw std::invalid_argument("data is null");
    if (count <= 0) return 0;
    if (count > maxDoubles_) throw std::invalid_argument("count > maxDoubles/payload cap");
    if (!isOpen_()) throw std::runtime_error("socket not open");

    if (timestampNanos == INT64_MIN) {
        timestampNanos = monotonicNowNanosNonNegative_();
    }

    const uint16_t n = (uint16_t)count;
    const size_t bytes = (size_t)HEADER_BYTES + (size_t)n * 8;

    uint8_t* buf = buffer_.data();

    // Header (matches Java exactly)
    writeBE32u_(buf + 0, MAGIC);
    writeBE16_ (buf + 4, VERSION);
    writeBE16_ (buf + 6, n);
    writeBE32_fromBits_(buf + 8, seq);
    writeBE64_fromBits_(buf + 12, timestampNanos);

    // Payload
    uint8_t* p = buf + HEADER_BYTES;
    for (uint16_t i = 0; i < n; ++i) {
        writeDoubleBE_(p + (size_t)i * 8, data[i]);
    }

    int sent;
    if (connect_) {
        sent = ::send(sock_, (const char*)buf, (int)bytes, 0);
    } else {
        sent = ::sendto(sock_, (const char*)buf, (int)bytes, 0,
                        (sockaddr*)&destAddr_, destAddrLen_);
    }

#if defined(_WIN32)
    if (sent == SOCKET_ERROR)
#else
    if (sent < 0)
#endif
        throw std::runtime_error("send/sendto failed: " + lastSockErr_());

    return (size_t)sent;
}

void UdpDoubleSender::close() { closeSock_(); }
