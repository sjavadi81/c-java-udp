package Javadi;


import javax.inject.Inject;

import Javadi.UdpDoubleSender;

import com.kuka.roboticsAPI.applicationModel.RoboticsAPIApplication;
import com.kuka.roboticsAPI.deviceModel.LBR;

/**
 * Implementation of a robot application.
 * <p>
 * The application provides a {@link RoboticsAPITask#initialize()} and a 
 * {@link RoboticsAPITask#run()} method, which will be called successively in 
 * the application lifecycle. The application will terminate automatically after 
 * the {@link RoboticsAPITask#run()} method has finished or after stopping the 
 * task. The {@link RoboticsAPITask#dispose()} method will be called, even if an 
 * exception is thrown during initialization or run. 
 * <p>
 * <b>It is imperative to call <code>super.dispose()</code> when overriding the 
 * {@link RoboticsAPITask#dispose()} method.</b> 
 * 
 * @see UseRoboticsAPIContext
 * @see #initialize()
 * @see #run()
 * @see #dispose()
 */
public class JUdpTest extends RoboticsAPIApplication {
    private UdpDoubleSender sender;
    
	@Inject
	private LBR lbr;

    @Override
    public void initialize() {
    	lbr = getContext().getDeviceFromType(LBR.class);
        try {
            // CHANGE THESE TO MATCH YOUR PC
            String pcIp = "172.31.1.150";
            int pcPort = 30005;
            int localPort = 30001;

            // Example: send up to 10 doubles per packet
            sender = new UdpDoubleSender(
                    pcIp,
                    pcPort,
                    localPort,
                    10
            );

        } catch (Exception e) {
            getLogger().error("Failed to initialize UDP sender", e);
        }
    }

    @Override
    public void run() {

        // Example data (replace with anything you want)
        double[] data = new double[] {
                1.0, 2.0, 3.0, 4.0, 5.0
        };

        long start = System.currentTimeMillis();

        while (!Thread.currentThread().isInterrupted()) {
            sender.send(data);

            if (System.currentTimeMillis() - start > 5000) { // 5 sec
                break;
            }

            try {
                Thread.sleep(10);
            } catch (InterruptedException e) {
                break;
            }
        }

    }

    @Override
    public void dispose() {
        if (sender != null) {
            sender.close();
        }
    }
}