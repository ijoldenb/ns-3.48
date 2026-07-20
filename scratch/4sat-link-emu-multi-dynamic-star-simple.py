import sys
import os
from ns import ns

# Global Tracking Variables
numNodes = 4
txDeviceToHub = [None] * numNodes
hubRxDevice = [None] * numNodes
traceFile = None

# --- 1. Pure Python Heartbeat Loop ---
def SimulationHeartbeat():
    """Keeps the Realtime Simulator engine awake and printing."""
    print(f"[⏱️ Simulator Alive] Time: {ns.Simulator.Now().GetSeconds()}s")
    sys.stdout.flush() # Force text to the screen instantly
    
    # Schedule the next heartbeat 1 second into the future
    ns.Simulator.Schedule(ns.Seconds(1.0), SimulationHeartbeat)

# --- 2. Python Trace Parser ---
def ParseNextNetworkXSnapshot():
    global traceFile
    line = traceFile.readline()
    if not line: return

    nextTimestamp = 0.0
    while line:
        tokens = line.strip().split()
        if tokens and tokens[0] == "TIMESTAMP":
            nextTimestamp = float(tokens[1])
            break
        line = traceFile.readline()

    line = traceFile.readline()
    while line:
        tokens = line.strip().split()
        if not tokens or tokens[0].startswith('#'):
            line = traceFile.readline()
            continue
        if tokens[0] == "END": break

        src, dst, bwMbps, dropRate = int(tokens[0]), int(tokens[1]), float(tokens[2]), float(tokens[3])

        if src < numNodes and txDeviceToHub[src] is not None:
            if bwMbps <= 0.0:
                bwMbps, dropRate = 0.000001, 1.0

            newRate = ns.DataRate(int(bwMbps * 1000000))
            txDeviceToHub[src].SetAttribute("DataRate", ns.DataRateValue(newRate))
            hubRxDevice[src].SetAttribute("DataRate", ns.DataRateValue(newRate))

            factory = ns.ObjectFactory()
            factory.SetTypeId("ns3::RateErrorModel")
            em = ns.cppyy.bind(factory.Create(), ns.RateErrorModel)
            em.SetAttribute("ErrorRate", ns.DoubleValue(dropRate))
            em.SetUnit(ns.RateErrorModel.ERROR_UNIT_PACKET)

            txDeviceToHub[src].SetAttribute("ReceiveErrorModel", ns.PointerValue(em))
            hubRxDevice[src].SetAttribute("ReceiveErrorModel", ns.PointerValue(em))
            
        line = traceFile.readline()

    print(f"[📈 Trace Update] Applied matrix at: {ns.Simulator.Now().GetSeconds()}s")
    sys.stdout.flush()
    
    timeDelta = max(0.001, nextTimestamp - ns.Simulator.Now().GetSeconds())
    ns.Simulator.Schedule(ns.Seconds(timeDelta), ParseNextNetworkXSnapshot)


# --- 3. Main Network Setup ---
def main():
    global traceFile
    print("🎬 Script started. Parsing command line...")
    sys.stdout.flush()
    
    cmd = ns.CommandLine()
    cmd.Parse(sys.argv)

    print("🔧 Configuring Realtime Simulator...")
    sys.stdout.flush()
    ns.GlobalValue.Bind("SimulatorImplementationType", ns.StringValue("ns3::RealtimeSimulatorImpl"))

    peripheralNodes = ns.NodeContainer()
    peripheralNodes.Create(numNodes)
    hubNode = ns.Node()

    # Central Hub Switch
    hubBridgeDevice = ns.BridgeNetDevice()
    hubNode.AddDevice(hubBridgeDevice)

    print("🔗 Building Point-to-Point Spokes...")
    sys.stdout.flush()
    for i in range(numNodes):
        p2p = ns.PointToPointHelper()
        p2p.SetDeviceAttribute("DataRate", ns.StringValue("100Mbps"))
        p2p.SetChannelAttribute("Delay", ns.StringValue("0ms"))

        linkNodes = ns.NodeContainer()
        linkNodes.Add(peripheralNodes.Get(i))
        linkNodes.Add(hubNode)
        
        devs = p2p.Install(linkNodes)
        devSpoke, devHubPort = devs.Get(0), devs.Get(1)

        qSize = ns.QueueSizeValue(ns.QueueSize("5000p"))
        devSpoke.GetQueue().SetAttribute("MaxSize", qSize)
        devHubPort.GetQueue().SetAttribute("MaxSize", qSize)

        txDeviceToHub[i] = devSpoke
        hubRxDevice[i] = devHubPort

        hubBridgeDevice.AddBridgePort(devHubPort)

    print("🔌 Binding TapBridges to External Linux VLANs...")
    sys.stdout.flush()
    vlanMapping = ["vlan101", "vlan102", "vlan103", "vlan104"]

    for i in range(numNodes):
        tapBridgeHelper = ns.TapBridgeHelper()
        tapBridgeHelper.SetAttribute("Mode", ns.StringValue("UseBridge"))
        tapBridgeHelper.SetAttribute("DeviceName", ns.StringValue(vlanMapping[i]))
        
        print(f"   👉 Mapping {vlanMapping[i]} to Node {i} Spoke Device...")
        sys.stdout.flush()
        tapBridgeHelper.Install(peripheralNodes.Get(i), txDeviceToHub[i])

    print("📂 Opening topology trace file...")
    sys.stdout.flush()
    try:
        traceFile = open("/home/ijoldenb/ns-3.48/scratch/topology_trace.txt", "r")
    except IOError:
        sys.exit("NS_FATAL_ERROR: Could not open trace file!")

    print("🚀 Scheduling event loops...")
    sys.stdout.flush()
    ns.Simulator.Schedule(ns.Seconds(0.0), ParseNextNetworkXSnapshot)
    ns.Simulator.Schedule(ns.Seconds(0.1), SimulationHeartbeat)

    ns.Simulator.Stop(ns.Seconds(3600.0))

    print("================================================================")
    print("ns-3 Realtime Emulation Initiated Successfully!")
    print("================================================================")
    sys.stdout.flush()

    ns.Simulator.Run()
    
    print("🏁 Simulation Finished Safely.")
    sys.stdout.flush()
    traceFile.close()
    ns.Simulator.Destroy()

if __name__ == '__main__':
    main()