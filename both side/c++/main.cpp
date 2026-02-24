#include "UdpDoubleReceiver.hpp"
#include "UdpDoubleSender.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdint>

int main()
{
    // ---- CONFIG ----
    const std::string localBindIp = "0.0.0.0";      // receive on all interfaces
    const uint16_t    recvPort    = 30002;          // local receive port (your requirement)

    const std::string remoteIp    = "172.31.1.147"; // other client
    const uint16_t    sendPort    = 30001;          // remote receive port (your requirement)

    const std::size_t rxBufferSize = 2048;
    const auto endian = UdpDoubleReceiver::Endian::Big; // MUST match sender packing (BE)

    const int maxDoubles = 64; // must be >= max doubles you will send
    // ----------------

    // ---- RECEIVER ----
    UdpDoubleReceiver receiver(localBindIp, (int)recvPort, rxBufferSize, endian);
    if (!receiver.start()) {
        std::cerr << "Failed to start UDP receiver on port " << recvPort << "\n";
        return 1;
    }

    // ---- SENDER (bind local port so replies come back to 30002) ----
    UdpDoubleSender sender(
        remoteIp,                    // remoteHost
        sendPort,                    // remotePort
        recvPort,                    // localPort (bind sender socket to 30002)
        maxDoubles,                  // maxDoubles
        true,                       // connectUdp
        UdpDoubleSender::DEFAULT_MAX_UDP_PAYLOAD,
        UdpDoubleSender::IpMode::IPv4,
        128 * 1024                   // requested send buffer
    );

    std::cout << "RX <- " << localBindIp << ":" << recvPort << "\n";
    std::cout << "TX -> " << remoteIp << ":" << sendPort << "\n";
    std::cout << "Running...\n";

    UdpDoubleReceiver::Packet pkt;

    int32_t seq = 1;

    while (true) {
        // ---- SEND ----
        std::vector<double> out = {
            (double)seq,
            123.456,
            -7.25,
            3.141592653589793
        };

        // Use your class API (no custom packing here)
        // timestampNanos default uses sender's monotonic clock when INT64_MIN, but we can also let it auto
        sender.sendWithSeq(out.data(), (int)out.size(), seq /*, timestampNanos */);

        // ---- RECEIVE (print latest) ----
        if (receiver.getLatest(pkt)) {
            std::cout << "RX SEQ=" << pkt.seq
                      << " t(ns)=" << pkt.timestampNanos
                      << " count=" << pkt.data.size()
                      << " data: ";
            for (double v : pkt.data) std::cout << v << " ";
            std::cout << "\n";
        }

        ++seq;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Not reached in this example
    // receiver.stop();
    // sender.close();
    // return 0;
}