package UsefullClasses;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * UDP sender for streaming arrays of doubles from Sunrise to a PC.
 *
 * Features:
 *  - One long-lived socket bound to a fixed local port
 *  - Preallocated buffer / ByteBuffer / DatagramPacket
 *  - Explicit endianness
 *  - MTU-friendly payload sizing (configurable)
 *  - Header: magic, version, seq, timestampNanos, count
 *  - Zero allocation in send()
 *
 * NOTE: Do not call from real-time SmartServo/FRI callbacks.
 */
public class UdpDoubleSender {

    // ---- Header format (all fields follow byteOrder) ----
    // int    magic          (0x55445044 = "UDPD")
    // short  version        (1)
    // short  count          (# of doubles)
    // int    sequence       (increments each send)
    // long   timestampNanos (System.nanoTime())
    //
    // Then: count doubles

    private static final int MAGIC = 0x55445044; // 'U''D''P''D'
    private static final short VERSION = 1;

    // Header size in bytes: 4 + 2 + 2 + 4 + 8 = 20
    private static final int HEADER_BYTES = 20;

    // Conservative safe UDP payload to avoid fragmentation on typical Ethernet.
    // (MTU 1500 - IP/UDP headers; 1400 is a common safe choice)
    private static final int DEFAULT_MAX_UDP_PAYLOAD = 1400;

    private final DatagramSocket socket;
    private final InetAddress remoteAddress;
    private final int remotePort;

    private final byte[] buffer;
    private final ByteBuffer bb;
    private final DatagramPacket packet;

    private final int maxDoubles;
    private final Object lock = new Object(); // thread-safety without allocations

    private int seq = 0;

    /**
     * @param remoteHost IP/hostname of the PC
     * @param remotePort UDP port on the PC (receiver binds here)
     * @param localPort  fixed local port on the robot (bind here)
     * @param maxDoubles requested max doubles per packet (will be capped by payload limit)
     * @param byteOrder  must match receiver (BIG_ENDIAN or LITTLE_ENDIAN)
     * @param connect    if true, socket.connect(remoteHost, remotePort)
     * @param maxPayloadBytes maximum UDP payload size (e.g., 1400). Use <= 1400 to avoid fragmentation.
     * 
     */
    public UdpDoubleSender(
            String remoteHost,
            int remotePort,
            int localPort,
            int maxDoubles,
            ByteOrder byteOrder,
            boolean connect,
            int maxPayloadBytes
    ) throws IOException {

        this.remoteAddress = InetAddress.getByName(remoteHost);
        this.remotePort = remotePort;

        // Bind to fixed local port
        DatagramSocket s = new DatagramSocket(null);
        try { s.setReuseAddress(true); } catch (Exception ignored) {}
        s.bind(new java.net.InetSocketAddress(localPort));
        this.socket = s;
        // Optional micro-optimizations
        try {
            socket.setSendBufferSize(128 * 1024);
            socket.setTrafficClass(0x10); // IPTOS_LOWDELAY
        } catch (Exception ignored) {}

        if (connect) {
            socket.connect(this.remoteAddress, this.remotePort);
        }

        // Cap payload to avoid fragmentation:
        // payload = HEADER_BYTES + (count * 8)
        int payloadLimit = (maxPayloadBytes > 0) ? maxPayloadBytes : DEFAULT_MAX_UDP_PAYLOAD;
        int maxByPayload = Math.max(0, (payloadLimit - HEADER_BYTES) / 8);

        this.maxDoubles = Math.max(0, Math.min(maxDoubles, maxByPayload));

        // Allocate a buffer for the maximum packet size we will send
        int maxPacketBytes = HEADER_BYTES + (this.maxDoubles * 8);
        this.buffer = new byte[maxPacketBytes];

        this.bb = ByteBuffer.wrap(buffer).order(byteOrder);

        // Initialize packet with full buffer length (not 0)
        this.packet = new DatagramPacket(buffer, buffer.length, remoteAddress, remotePort);
    }

    /** Convenience constructor: BIG_ENDIAN, connect=false, payload=1400 */
    public UdpDoubleSender(String remoteHost, int remotePort, int localPort, int maxDoubles) throws IOException {
        this(remoteHost, remotePort, localPort, maxDoubles, ByteOrder.BIG_ENDIAN, false, DEFAULT_MAX_UDP_PAYLOAD);
    }

    /** Send all doubles (up to maxDoubles). */
    public void send(double[] data) {
        if (data == null || data.length == 0) return;
        send(data, data.length);
    }

    /** Send first 'length' doubles. */
    public void send(double[] data, int length) {
        if (data == null || length <= 0) return;
        if (socket == null || socket.isClosed()) return;

        final int n = Math.min(Math.min(length, data.length), maxDoubles);
        if (n <= 0) return;

        synchronized (lock) {
            bb.clear();

            // Header
            bb.putInt(MAGIC);
            bb.putShort(VERSION);
            bb.putShort((short) n);
            bb.putInt(seq++);
            bb.putLong(System.nanoTime());

            // Payload
            for (int i = 0; i < n; i++) {
                bb.putDouble(data[i]);
            }

            int byteCount = HEADER_BYTES + (n * 8);

            packet.setLength(byteCount);   // <-- change here
            try {
                socket.send(packet);
            } catch (IOException ignored) {
            }
        }
    }

    public int getMaxDoubles() {
        return maxDoubles;
    }
    
    public void resetSeq() {
        synchronized (lock) {
            seq = 0;
        }
    }

    public void setSeq(int newSeq) {
        synchronized (lock) {
            seq = newSeq;
        }
    }

    public void close() {
        if (socket != null && !socket.isClosed()) {
            socket.close();
        }
    }
}
