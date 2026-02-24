package UsefullClasses;

import javax.inject.Inject;
import java.nio.ByteOrder;
import java.util.concurrent.TimeUnit;

import com.kuka.roboticsAPI.applicationModel.tasks.RoboticsAPICyclicBackgroundTask;
import com.kuka.roboticsAPI.applicationModel.tasks.CycleBehavior;
import com.kuka.task.ITaskLogger;

public class UdpSenderBackground extends RoboticsAPICyclicBackgroundTask {

    @Inject
    private ITaskLogger logger;

    private UdpDoubleSender sender;

    // CONFIG
    private String remoteIp = "172.31.1.150";
    private int remotePort = 30001;
    private int localPort  = 30003;

    private long cycleMs = 10; // 100 Hz
    private CycleBehavior behavior = CycleBehavior.BestEffort;

    // Telemetry buffer (written by your app thread)
    private final double[] telemetry = new double[8];

    // Preallocated TX buffer (used only by this background task thread)
    private final double[] txBuf = new double[telemetry.length];

    private final Object telemetryLock = new Object();

    @Override
    public void initialize() {
        initializeCyclic(0L, cycleMs, TimeUnit.MILLISECONDS, behavior);

        try {
            sender = new UdpDoubleSender(
                    remoteIp, remotePort, localPort,
                    telemetry.length,
                    ByteOrder.BIG_ENDIAN,
                    true,
                    1400
            );

            if (logger != null) {
                logger.info("UDP TX init: " + remoteIp + ":" + remotePort
                        + " localPort=" + localPort
                        + " cycleMs=" + cycleMs);
            }

        } catch (Exception e) {
            if (logger != null) logger.error("UDP TX init failed", e);
            throw new RuntimeException(e);
        }
    }

    // Update one value (no allocations)
    public void updateTelemetry(int i, double v) {
        if (i < 0 || i >= telemetry.length) return;
        synchronized (telemetryLock) {
            telemetry[i] = v;
        }
    }

    // Update all values at once (no allocations)
    public void updateTelemetry(double[] src) {
        if (src == null) return;
        synchronized (telemetryLock) {
            System.arraycopy(src, 0, telemetry, 0, Math.min(src.length, telemetry.length));
        }
    }

    @Override
    public void runCyclic() {
        if (sender == null) return;

        // Snapshot copy so we hold the lock briefly, then send without holding it
        synchronized (telemetryLock) {
            System.arraycopy(telemetry, 0, txBuf, 0, telemetry.length);
        }

        sender.send(txBuf);
    }

    @Override
    public void dispose() {
        try { if (sender != null) sender.close(); } catch (Exception ignored) {}
        if (logger != null) logger.info("UDP TX disposed");
    }

    // Optional setters
    public void setRemoteIp(String ip) { this.remoteIp = ip; }
    public void setCycleMs(long ms) { this.cycleMs = ms; }
}