import sys
from ns import ns 

# --- Global Configurations ---
numNodes = 4

# Tracking Matrices (Initialized to None)
# meshDevices[SrcNode][DstNode]
meshDevices = [[None for _ in range(numNodes)] for _ in range(numNodes)]
g_fdDev = [None for _ in range(numNodes)]

# Global MAC Table for distributed switching
macToNodeMap = {}


# --- 1. Custom Encapsulation Tunnel ---
def SendOverP2PTunnel(dev, packet, protocol, src_addr, dst_addr):
    pktCopy = packet.Copy()
    eth = ns.EthernetHeader()
    eth.SetSource(ns.Mac48Address.ConvertFrom(src_addr))
    eth.SetDestination(ns.Mac48Address.ConvertFrom(dst_addr))
    eth.SetLengthType(protocol)
    
    pktCopy.AddHeader(eth)
    dev.Send(pktCopy, dev.GetBroadcast(), 0x0800)


# --- 2. Distributed Switch Ingress (From Physical Raspberry Pi) ---
def make_vlan_ingress_callback(myNodeID):
    def Ingress_From_Vlan(rxDevice, packet, protocol, src, dst, packetType):
        srcMac = ns.Mac48Address.ConvertFrom(src)
        dstMac = ns.Mac48Address.ConvertFrom(dst)

        # Learn where this Pi's MAC address lives
        if not srcMac.IsBroadcast():
            macToNodeMap[str(srcMac)] = myNodeID

        # If Broadcast/Multicast (like ARP or OSPF), flood to all OTHER nodes in the mesh
        if dstMac.IsBroadcast() or dstMac.IsGroup():
            for i in range(numNodes):
                if i != myNodeID and meshDevices[myNodeID][i] is not None:
                    SendOverP2PTunnel(meshDevices[myNodeID][i], packet, protocol, src, dst)
        else: # Unicast
            dst_str = str(dstMac)
            if dst_str in macToNodeMap:
                targetNode = macToNodeMap[dst_str]
                # Forward directly down the dedicated mesh wire to the destination
                if targetNode != myNodeID and meshDevices[myNodeID][targetNode] is not None:
                    SendOverP2PTunnel(meshDevices[myNodeID][targetNode], packet, protocol, src, dst)
            else:
                # Unknown unicast: Flood to all other nodes until MAC is learned
                for i in range(numNodes):
                    if i != myNodeID and meshDevices[myNodeID][i] is not None:
                        SendOverP2PTunnel(meshDevices[myNodeID][i], packet, protocol, src, dst)
        return True
    return Ingress_From_Vlan


# --- 3. Split-Horizon Ingress (From ns-3 Mesh) ---
def make_mesh_ingress_callback(myNodeID):
    def Ingress_From_Mesh(rxDevice, packet, protocol, src, dst, packetType):
        pktCopy = packet.Copy()
        eth = ns.EthernetHeader()
        pktCopy.RemoveHeader(eth)
        
        # SPLIT HORIZON RULE: Packets arriving from the mesh are NEVER forwarded back into the mesh.
        # They are only sent UP to the physical Raspberry Pi attached to this node.
        g_fdDev[myNodeID].Send(pktCopy, eth.GetDestination(), eth.GetLengthType())
        
        return True
    return Ingress_From_Mesh


# --- 4. Scheduled Apply Function (Strict Directional Mapping) ---
def ApplyLinkChanges(changes):
    print(f"\n--- Sim Time: {ns.Simulator.Now().GetSeconds()}s | Applying YAML Topology Updates ---")
    for change in changes:
        src = change['src']
        dst = change['dst']
        
        if src < numNodes and dst < numNodes and src != dst:
            bw = 0.000001 if change['bwMbps'] <= 0.0 else change['bwMbps']
            drop = 1.0 if change['bwMbps'] <= 0.0 else change['dropRate']

            newRate = ns.DataRate(f"{bw}Mbps")
            
            # Create error model for Destination node receiving from Source
            emDst = ns.RateErrorModel()
            emDst.SetAttribute("ErrorRate", ns.DoubleValue(drop))
            emDst.SetUnit(ns.RateErrorModel.ERROR_UNIT_PACKET)

            # Create error model for Source node receiving from Destination
            emSrc = ns.RateErrorModel()
            emSrc.SetAttribute("ErrorRate", ns.DoubleValue(drop))
            emSrc.SetUnit(ns.RateErrorModel.ERROR_UNIT_PACKET)

            # 1. Throttle Forward Path (Src -> Dst)
            meshDevices[src][dst].SetAttribute("DataRate", ns.DataRateValue(newRate))
            meshDevices[dst][src].SetAttribute("ReceiveErrorModel", ns.PointerValue(emDst))
            
            # 2. Throttle Return Path (Dst -> Src)
            meshDevices[dst][src].SetAttribute("DataRate", ns.DataRateValue(newRate))
            meshDevices[src][dst].SetAttribute("ReceiveErrorModel", ns.PointerValue(emSrc))

            print(f"  [Link Updated Symmetrically] {src + 1} <-> {dst + 1} | BW: {bw} Mbps | Drop Rate: {drop}")


def KeepAliveDummyEvent():
    pass


# --- 5. Main Network Setup ---
def main():
    ns.CommandLine().Parse(sys.argv)
    ns.GlobalValue.Bind("SimulatorImplementationType", ns.StringValue("ns3::RealtimeSimulatorImpl"))

    meshNodes = ns.NodeContainer()
    meshNodes.Create(numNodes)

    p2p = ns.PointToPointHelper()
    p2p.SetDeviceAttribute("DataRate", ns.StringValue("100Mbps"))
    p2p.SetChannelAttribute("Delay", ns.StringValue("0ms"))
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", ns.QueueSizeValue(ns.QueueSize("5000p")))

    # Build the Full Mesh (Creates N*(N-1)/2 Links)
    for i in range(numNodes):
        for j in range(i + 1, numNodes):
            linkNodes = ns.NodeContainer()
            linkNodes.Add(meshNodes.Get(i))
            linkNodes.Add(meshNodes.Get(j))
            devs = p2p.Install(linkNodes)

            devI = devs.Get(0)
            devJ = devs.Get(1)

            meshDevices[i][j] = devI # Device on 'i' transmitting to 'j'
            meshDevices[j][i] = devJ # Device on 'j' transmitting to 'i'

            # Bind the receiving callbacks using closures
            devI.SetPromiscReceiveCallback(ns.NetDevice.PromiscReceiveCallback(make_mesh_ingress_callback(i)))
            devJ.SetPromiscReceiveCallback(ns.NetDevice.PromiscReceiveCallback(make_mesh_ingress_callback(j)))

    emuHelper = ns.fd_net_device.EmuFdNetDeviceHelper()
    emuHelper.SetAttribute("EncapsulationMode", ns.StringValue("Dix"))
    vlanMapping = ["vlan101", "vlan102", "vlan103", "vlan104"]

    for i in range(numNodes):
        emuHelper.SetDeviceName(vlanMapping[i])
        
        devSide = emuHelper.Install(meshNodes.Get(i))
        g_fdDev[i] = devSide.Get(0)
        
        mac_addr = ns.Mac48Address.Allocate()
        g_fdDev[i].SetAttribute("Address", ns.Mac48AddressValue(mac_addr))

        # Bind the Pi ingest callback using closures
        g_fdDev[i].SetPromiscReceiveCallback(ns.NetDevice.PromiscReceiveCallback(make_vlan_ingress_callback(i)))

    # --- Parse YAML File ---
    timelineMap = {}
    scheduleTime = 0.0
    inLink = False
    tempChange = {}

    trace_file_path = "/home/ijoldenb/ns-3.48/scratch/topology_trace.yaml"
    
    try:
        with open(trace_file_path, "r") as traceFile:
            for line in traceFile:
                line = line.strip()
                if not line or ":" not in line:
                    continue
                
                key, valStr = [x.strip() for x in line.split(":", 1)]

                if "- time" in key:
                    scheduleTime = float(valStr)
                elif "- src" in key:
                    # Subtract 1 to map YAML Node 1-4 to internal Node 0-3
                    tempChange['src'] = int(valStr) - 1
                    inLink = True
                elif inLink and "dst" in key:
                    # Subtract 1 to map YAML Node 1-4 to internal Node 0-3
                    tempChange['dst'] = int(valStr) - 1
                elif inLink and "bw" in key:
                    tempChange['bwMbps'] = float(valStr)
                elif inLink and "drop" in key:
                    tempChange['dropRate'] = float(valStr)
                    
                    if scheduleTime not in timelineMap:
                        timelineMap[scheduleTime] = []
                    timelineMap[scheduleTime].append(tempChange.copy())
                    
                    inLink = False
                    tempChange = {}
    except FileNotFoundError:
        print(f"FATAL ERROR: Could not open {trace_file_path}!")
        sys.exit(1)

    # Schedule the link modifications
    for t_time, changes in timelineMap.items():
        ns.Simulator.Schedule(ns.Seconds(t_time), ApplyLinkChanges, changes)

    stopTime = ns.Seconds(3600.0)
    ns.Simulator.Stop(stopTime)
    ns.Simulator.Schedule(stopTime - ns.Seconds(1.0), KeepAliveDummyEvent)

    print("================================================================")
    print("ns-3 Dynamic PointToPoint FULL MESH Active (Python L2 Split-Horizon)")
    print("================================================================")

    ns.Simulator.Run()
    ns.Simulator.Destroy()

if __name__ == '__main__':
    main()