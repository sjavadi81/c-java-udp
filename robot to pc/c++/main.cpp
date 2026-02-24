#include "UdpDoubleReceiver.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    // --- CONFIG ---
    const std::string bindIp = "0.0.0.0";   // listen on all interfaces
    const int port = 30005;                 // must match Java sender remotePort
    const std::size_t bufferSize = 2048;    // plenty for header + data
    const auto endian = UdpDoubleReceiver::Endian::Big; // MUST match Java sender

    // --- CREATE RECEIVER ---
    UdpDoubleReceiver receiver(bindIp, port, bufferSize, endian);

    if (!receiver.start()) {
        std::cerr << "Failed to start UDP receiver\n";
        return 1;
    }

    std::cout << "Waiting for UDP data...\n";

    UdpDoubleReceiver::Packet pkt;

    // --- MAIN LOOP ---
    while (true) {
        if (receiver.getLatest(pkt)) {

            std::cout << "SEQ=" << pkt.seq
                      << "  t(ns)=" << pkt.timestampNanos
                      << "  count=" << pkt.data.size()
                      << "  data: ";

            for (double v : pkt.data) {
                std::cout << v << " ";
            }

            std::cout << "\n";
        }

        // Avoid busy spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    // Never reached in this example
    // receiver.stop();

    return 0;
}
