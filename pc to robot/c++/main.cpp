
#include "UdpDoubleSernder.hpp"
#include <chrono>
#include <thread>
#include <iostream>

int main() {

    std::cout << "UDP Double Sender Example\n"; 
    try {
        // Robot controller IP (KRC / Sunrise cabinet) on your robot network:
        // CHANGE THIS to the robot’s IP on the 172.31.1.x network.
        const std::string robotIp = "172.31.1.147";   // <-- example, replace
        const uint16_t robotPort  = 30002;          // matches Sunrise receiver
        const uint16_t pcLocalPort = 30001;         // optional (0 = OS chooses)
        const int maxDoubles = 172;                 // matches receiver config

        // Create sender (unicast)
        UdpDoubleSernder tx(robotIp, robotPort, pcLocalPort, maxDoubles);

        // Important if you ever route (usually not needed on direct robot LAN):
        tx.setUnicastHopLimit(64);

        // Send loop ~2ms to match your background task rate
        using clock = std::chrono::steady_clock;
        auto next = clock::now();

        double packet[2] = {0.0, 0.0};

        for (int i = 0; i < 20000; ++i) {
            // Example signal: X,Y
            packet[0] = 0.100 * i;   // meters? whatever your xyScale expects
            packet[1] = 0.050 * i;

            // This increments seq internally, so STRICT_INCREASING is satisfied.
            tx.sendAutoSeq(packet, 2);

            next += std::chrono::milliseconds(2);
            std::this_thread::sleep_until(next);
        }

        std::cout << "Done sending.\n";
        tx.close();
    } catch (const std::exception& e) {
        std::cerr << "Sender error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}