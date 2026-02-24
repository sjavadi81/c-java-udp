package UsefullClasses;

import javax.inject.Inject;
import java.util.concurrent.TimeUnit;

import com.kuka.roboticsAPI.applicationModel.tasks.RoboticsAPICyclicBackgroundTask;
import com.kuka.roboticsAPI.applicationModel.tasks.CycleBehavior;
import com.kuka.task.ITaskLogger;

/**
 * Fast UDP receiver task for online control.
 * Runs receiveOnce() frequently with a very small SO_TIMEOUT to keep latency low.
 *
 * IMPORTANT:
 * - Do NOT do any robot motion here.
 * - Motion code reads receiver.readLatestView(...) with maxAgeNanos gating.
 */
public class UdpReceiverBackground extends RoboticsAPICyclicBackgroundTask {

    @Inject
    private ITaskLogger logger;

    private UdpDoubleReceiver receiver;

    // ---------- CONFIG ----------
    private int localPort = 30002;
    private int maxDoubles = 172;       // for 1400 payload. set to your sender max.
    private int maxPayloadBytes = 1400;

    // CRITICAL on robot side: set the sender PC IP
    private String allowedSenderIp = "172.31.1.150";   // <-- CHANGE THIS
    private int allowedSenderPort = 0;

    // Low-latency polling:
    private long cycleMs = 4L;          // 4ms cycle (try 4ms if CPU load is high)
    private int soTimeoutMs = 1;        // must be <= cycleMs generally

    private CycleBehavior behavior = CycleBehavior.BestEffort;
    // If BestEffort doesn't exist in your Sunrise version, use IDE autocomplete and pick the closest.

    // Health log (keep it low rate)
    private long lastLogMs = 0L;
    private static final long LOG_INTERVAL_MS = 2000L; // 2 seconds

    // Optional: detect no data for too long (diagnostics only; your motion loop must also gate!)
    private static final long WARN_STALE_MS = 200L;

    @Override
    public void initialize() {
        initializeCyclic(0L, cycleMs, TimeUnit.MILLISECONDS, behavior);

        try {
            receiver = new UdpDoubleReceiver(
                    localPort,
                    maxDoubles,
                    maxPayloadBytes,
                    allowedSenderIp,
                    allowedSenderPort,
                    soTimeoutMs,
                    UdpDoubleReceiver.SeqPolicy.STRICT_INCREASING,
                    0L,
                    false,
                    2000
            );

            if (logger != null) {
                logger.info("UdpOnlineControlRxTask init: port=" + localPort
                        + " cycleMs=" + cycleMs
                        + " soTimeoutMs=" + soTimeoutMs
                        + " sender=" + allowedSenderIp);
            }

        } catch (Exception e) {
            if (logger != null) logger.error("UDP RX init failed", e);
            throw new RuntimeException(e);
        }
    }

    @Override
    public void runCyclic() {
        UdpDoubleReceiver r = receiver;
        if (r == null) return;

        r.receiveOnce();

        long nowMs = System.currentTimeMillis();
        if (nowMs - lastLogMs >= LOG_INTERVAL_MS) {
            lastLogMs = nowMs;

            long ageMs = ageMs(r.getLatestLocalRxTimeNanos());
            if (logger != null) {
                logger.info("UDP: ok=" + r.getPacketsOk()
                        + " lost=" + r.getPacketsLost()
                        + " late=" + r.getPacketsLateOrDup()
                        + " invalid=" + r.getPacketsInvalid()
                        + " ageMs=" + ageMs
                        + " seq=" + r.getLatestSeq()
                        + " cnt=" + r.getLatestCount());
                if (ageMs >= 0 && ageMs > WARN_STALE_MS) {
                    logger.warn("UDP appears stale (ageMs=" + ageMs + "). Motion loop should fall back safely.");
                }
            }
        }
    }

    @Override
    public void dispose() {
        try {
            if (receiver != null) receiver.close();
        } catch (Exception ignored) {}
        if (logger != null) logger.info("UdpOnlineControlRxTask disposed");
    }

    public UdpDoubleReceiver getReceiver() {
        return receiver;
    }

    private long ageMs(long localRxTimeNanos) {
        if (localRxTimeNanos == Long.MIN_VALUE) return -1L;
        return (System.nanoTime() - localRxTimeNanos) / 1000000L;
    }

    // Optional setters (if you want to configure from app)
    public void setAllowedSenderIp(String ip) { this.allowedSenderIp = ip; }
    public void setCycleMs(long ms) { this.cycleMs = ms; }
    public void setSoTimeoutMs(int ms) { this.soTimeoutMs = ms; }
}