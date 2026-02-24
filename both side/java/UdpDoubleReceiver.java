package UsefullClasses;

import java.io.IOException;
import java.net.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public final class UdpDoubleReceiver {

    private static final int MAGIC = 0x55445044; // "UDPD"
    private static final short VERSION = 1;
    private static final int HEADER_BYTES = 20;

    // BIG_ENDIAN baked in:
    private static final ByteOrder ORDER = ByteOrder.BIG_ENDIAN;

    public enum SeqPolicy {
        STRICT_INCREASING,
        ALLOW_RESET_AFTER_QUIET
    }

    /** Caller-owned container for zero-copy reads (DO NOT MODIFY snapshot.data contents). */
    public static final class Snapshot {
        public double[] data;
        public int count;
        public int seq;
        public long senderTimestampNanos;
        public long localRxTimeNanos;
        public long packetsLostTotal;
    }

    private final DatagramSocket socket;
    private final InetAddress allowedSender; // nullable
    private final int allowedSenderPort;     // 0 => any
    private final int maxDoubles;

    private final byte[] rxBytes;
    private final DatagramPacket rxPacket;
    private final ByteBuffer rxBB; // wraps rxBytes (zero-copy parse)
    // Double buffers
    private final double[] bufA;
    private final double[] bufB;

    // Published (volatile for lock-free reads)
    private volatile double[] publishedBuf;
    private volatile int publishedCount = 0;
    private volatile int publishedSeq = -1;
    private volatile long publishedSenderTs = Long.MIN_VALUE;
    private volatile long publishedLocalRxTime = Long.MIN_VALUE;

    // Diagnostics
    private volatile long packetsOk = 0;
    private volatile long packetsInvalid = 0;
    private volatile long packetsRejectedSender = 0;
    private volatile long packetsLateOrDup = 0;
    private volatile long packetsLost = 0;

    // Policy
    private final SeqPolicy seqPolicy;
    private final long resetQuietNanos;

    // Flood protection
    private final boolean floodProtection;
    private final int floodMaxPacketsPerSecond;
    private long floodSecStart = System.nanoTime();
    private int floodSecCount = 0;

    public UdpDoubleReceiver(
            int localPort,
            int maxDoubles,
            int maxPayloadBytes,
            String allowedSenderIp,
            int allowedSenderPort,
            int soTimeoutMs,
            SeqPolicy seqPolicy,
            long resetQuietNanos,
            boolean floodProtection,
            int floodMaxPacketsPerSecond
    ) throws IOException {

        this.maxDoubles = Math.max(0, maxDoubles);

        this.allowedSender = (allowedSenderIp == null || allowedSenderIp.length() == 0)
                ? null
                : InetAddress.getByName(allowedSenderIp);

        this.allowedSenderPort = Math.max(0, allowedSenderPort);

        int payload = (maxPayloadBytes > 0) ? maxPayloadBytes : 1400;
        int bufSize = Math.max(HEADER_BYTES, payload);

        this.rxBytes = new byte[bufSize];
        this.rxPacket = new DatagramPacket(rxBytes, rxBytes.length);
        this.rxBB = ByteBuffer.wrap(rxBytes).order(ORDER); 
        this.bufA = new double[this.maxDoubles];
        this.bufB = new double[this.maxDoubles];
        this.publishedBuf = bufA;

        this.seqPolicy = (seqPolicy == null) ? SeqPolicy.STRICT_INCREASING : seqPolicy;
        this.resetQuietNanos = Math.max(0L, resetQuietNanos);

        this.floodProtection = floodProtection;
        this.floodMaxPacketsPerSecond = Math.max(1, floodMaxPacketsPerSecond);

        this.socket = new DatagramSocket(null);
        try { socket.setReuseAddress(true); } catch (Exception ignored) {}
        socket.bind(new InetSocketAddress(localPort));

        // Sunrise-friendly
        try { socket.setReceiveBufferSize(32 * 1024); } catch (Exception ignored) {}
        try { socket.setSoTimeout(Math.max(1, soTimeoutMs)); } catch (Exception ignored) {}
    }

    /** Convenience defaults: strict seq, payload 1400, timeout 100ms, no sender filter. */
    public UdpDoubleReceiver(int localPort, int maxDoubles) throws IOException {
        this(localPort, maxDoubles, 1400, null, 0, 100,
                SeqPolicy.STRICT_INCREASING, 0L,
                false, 2000);
    }

    private boolean isAllowedSender(DatagramPacket p) {
        if (allowedSender != null && !allowedSender.equals(p.getAddress())) return false;
        if (allowedSenderPort > 0 && p.getPort() != allowedSenderPort) return false;
        return true;
    }

    /**
     * Do ONE receive attempt (blocking up to soTimeout).
     * @return 1 accepted/published, 0 timeout/no packet, -1 received but rejected/invalid/error.
     */
    public int receiveOnce() {
        try {
            socket.receive(rxPacket);
            final long localNow = System.nanoTime();

            // Flood protection (simple per-second limiter)
            if (floodProtection) {
                if (localNow - floodSecStart >= 1000000000L) {
                    floodSecStart = localNow;
                    floodSecCount = 0;
                }
                floodSecCount++;
                if (floodSecCount > floodMaxPacketsPerSecond) {
                    return -1; // drop without sleeping
                }
            }

            if (!isAllowedSender(rxPacket)) {
                packetsRejectedSender++;
                return -1;
            }

            int len = rxPacket.getLength();
            if (len < HEADER_BYTES) {
                packetsInvalid++;
                return -1;
            }

            int off = rxPacket.getOffset(); // usually 0
            rxBB.clear();
            rxBB.position(off);
            rxBB.limit(off + len);
            
            // now parse directly from rxBytes
            int magic = rxBB.getInt();
            short ver = rxBB.getShort();
            int count = rxBB.getShort() & 0xFFFF;
            int seq = rxBB.getInt();
            long senderTs = rxBB.getLong();

            if (magic != MAGIC || ver != VERSION) { packetsInvalid++; return -1; }
            if (count <= 0 || count > maxDoubles) { packetsInvalid++; return -1; }
            if (rxBB.remaining() < count * 8) { packetsInvalid++; return -1; }

            // Sequence policy (timestamp informational only)
            int prevSeq = publishedSeq;
            long prevLocalRx = publishedLocalRxTime;

            boolean accept;
            if (prevSeq < 0) {
                accept = true;
            } else if (seq > prevSeq) {
                accept = true;
                int gap = seq - prevSeq;
                if (gap > 1) packetsLost += (long)(gap - 1);
            } else {
                if (seqPolicy == SeqPolicy.ALLOW_RESET_AFTER_QUIET
                        && prevLocalRx != Long.MIN_VALUE
                        && (localNow - prevLocalRx) >= resetQuietNanos) {
                    accept = true; // stream reset
                } else {
                    accept = false;
                }
            }

            if (!accept) {
                packetsLateOrDup++;
                return -1;
            }

            // Decode into non-published buffer, then publish by swapping reference
            double[] target = (publishedBuf == bufA) ? bufB : bufA;
            for (int i = 0; i < count; i++) {
                target[i] = rxBB.getDouble();
            }

         // Publish buffer first, then metadata, and seq LAST as "commit"
            publishedBuf = target;
            publishedCount = count;
            publishedSenderTs = senderTs;
            publishedLocalRxTime = localNow;
            publishedSeq = seq; // LAST

            packetsOk++;
            return 1;

        } catch (SocketTimeoutException timeout) {
            return 0;
        } catch (IOException io) {
            packetsInvalid++;
            return -1;
        } catch (Throwable t) {
            packetsInvalid++;
            return -1;
        }
    }

    // ---------- Read APIs (non-blocking) ----------

    public int readLatestView(Snapshot snapshot, long maxAgeNanos) {
        if (snapshot == null) return 0;

        int s1 = publishedSeq;
        if (s1 < 0) return 0;

        int n = publishedCount;
        if (n <= 0) return 0;

        long rxTime = publishedLocalRxTime;
        if (maxAgeNanos > 0L && rxTime != Long.MIN_VALUE) {
            long age = System.nanoTime() - rxTime;
            if (age > maxAgeNanos) return 0;
        }

        double[] buf = publishedBuf;

        // Fill snapshot
        snapshot.data = buf; // DO NOT MODIFY
        snapshot.count = n;
        snapshot.seq = s1;
        snapshot.senderTimestampNanos = publishedSenderTs;
        snapshot.localRxTimeNanos = rxTime;
        snapshot.packetsLostTotal = packetsLost;

        int s2 = publishedSeq;
        if (s1 != s2) return 0; // changed during read; try again next cycle

        return n;
    }

    public int readLatestInto(double[] dst, long maxAgeNanos) {
        if (dst == null) return -1;

        int s1 = publishedSeq;
        if (s1 < 0) return 0;

        int n = publishedCount;
        if (n <= 0) return 0;
        if (dst.length < n) return -1;

        long rxTime = publishedLocalRxTime;
        if (maxAgeNanos > 0L && rxTime != Long.MIN_VALUE) {
            long age = System.nanoTime() - rxTime;
            if (age > maxAgeNanos) return 0;
        }

        double[] buf = publishedBuf;
        System.arraycopy(buf, 0, dst, 0, n);

        int s2 = publishedSeq;
        if (s1 != s2) return 0;

        return n;
    }

    // ---------- Diagnostics ----------
    public int getLatestSeq() { return publishedSeq; }
    public long getLatestLocalRxTimeNanos() { return publishedLocalRxTime; }
    public int getLatestCount() { return publishedCount; }

    public long getPacketsOk() { return packetsOk; }
    public long getPacketsInvalid() { return packetsInvalid; }
    public long getPacketsRejectedSender() { return packetsRejectedSender; }
    public long getPacketsLateOrDup() { return packetsLateOrDup; }
    public long getPacketsLost() { return packetsLost; }

    public void close() {
        try { socket.close(); } catch (Exception ignored) {}
    }
}