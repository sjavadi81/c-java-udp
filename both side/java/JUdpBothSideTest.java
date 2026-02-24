package Javadi;

import UsefullClasses.UdpDoubleReceiver;
import UsefullClasses.UdpDoubleSender;

import com.kuka.common.ThreadUtil;
import com.kuka.roboticsAPI.applicationModel.RoboticsAPIApplication;

import java.nio.ByteOrder;

public class JUdpBothSideTest extends RoboticsAPIApplication {

    private static final String REMOTE_IP = "172.31.1.150";
    private static final int MAX_DOUBLES = 64;
    
    private static final int RECV_PORT = 30002; // robot listens here
    private static final int SEND_PORT = 30001; // PC listens here
    // ------------------------------------------------

    private UdpDoubleReceiver receiver;
    private UdpDoubleSender sender;

    private final UdpDoubleReceiver.Snapshot snap =
            new UdpDoubleReceiver.Snapshot();

    @Override
    public void initialize() {

        try {
            // Receiver: bind to 30002
            receiver = new UdpDoubleReceiver(
                    RECV_PORT,
                    MAX_DOUBLES,
                    2048,
                    null,          // accept any sender (like 0.0.0.0 in C++)
                    0,
                    50,
                    UdpDoubleReceiver.SeqPolicy.STRICT_INCREASING,
                    0L,
                    false,
                    2000
            );

            // Sender: bind ALSO to 30002 (same as C++ example)
            sender = new UdpDoubleSender(
                    REMOTE_IP,
                    SEND_PORT,
                    0,          // local port = ephemeral (do NOT bind)
                    MAX_DOUBLES,
                    ByteOrder.BIG_ENDIAN,
                    true,
                    1400
            );

            getLogger().info("RX <- 0.0.0.0:" + RECV_PORT);
            getLogger().info("TX -> " + REMOTE_IP + ":" + SEND_PORT);
            getLogger().info("Running...");

        } catch (Exception e) {
            throw new RuntimeException("UDP init failed", e);
        }
    }

    @Override
    public void run() {

        while (true) {

            // Receive one packet
            receiver.receiveOnce();

            // Read latest packet
            int n = receiver.readLatestView(snap, 0L);

            if (n > 0) {

                // Log
                StringBuilder sb = new StringBuilder();
                sb.append("RX SEQ=").append(snap.seq)
                  .append(" count=").append(snap.count)
                  .append(" data: ");

                for (int i = 0; i < snap.count; i++) {
                    sb.append(snap.data[i]).append(" ");
                }

                getLogger().info(sb.toString());

                // Echo exact doubles back to PC
                sender.send(snap.data, snap.count);
            }

            ThreadUtil.milliSleep(2);
        }
    }

    @Override
    public void dispose() {
        try { if (receiver != null) receiver.close(); } catch (Exception ignored) {}
        try { if (sender != null) sender.close(); } catch (Exception ignored) {}
        getLogger().info("UDP stopped");
    }
}