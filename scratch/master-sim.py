import sys
import socket
import json
from ns import ns
import cppyy

# ==============================================================================
# --- 0. CRITICAL C++ TO PYTHON CALLBACK BRIDGE & CASTING ENGINE ---
# ==============================================================================
cppyy.cppdef("""
#include "ns3/net-device.h"
#include "ns3/packet.h"
#include "ns3/callback.h"
#include "ns3/simulator.h"
#include "ns3/event-impl.h"
#include "ns3/mac48-address.h"
#include "ns3/address.h"
#include <functional>
#include <vector>
#include <memory>

namespace ns3 {

class PromiscCallbackBridge {
public:
    std::function<bool(Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&, const Address&, NetDevice::PacketType)> m_pyFunc;

    PromiscCallbackBridge(std::function<bool(Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&, const Address&, NetDevice::PacketType)> pyFunc)
        : m_pyFunc(pyFunc) {}

    bool Invoke(Ptr<NetDevice> dev, Ptr<const Packet> pkt, uint16_t proto, const Address& src, const Address& dst, NetDevice::PacketType type) {
        return m_pyFunc(dev, pkt, proto, src, dst, type);
    }
};

static std::vector<std::shared_ptr<PromiscCallbackBridge>> g_callbackBridges;

Callback<bool, Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&, const Address&, NetDevice::PacketType>
CreatePromiscCallback(std::function<bool(Ptr<NetDevice>, Ptr<const Packet>, uint16_t, const Address&, const Address&, NetDevice::PacketType)> pyFunc) {
    auto bridge = std::make_shared<PromiscCallbackBridge>(pyFunc);
    g_callbackBridges.push_back(bridge);
    return MakeCallback(&PromiscCallbackBridge::Invoke, bridge.get());
}

class PythonEventImpl : public EventImpl {
public:
    std::function<void()> m_func;
    PythonEventImpl(std::function<void()> func) : m_func(func) {}
    virtual void Notify() override {
        m_func();
    }
};

void SchedulePythonEvent(Time delay, std::function<void()> func) {
    Simulator::Schedule(delay, Create<PythonEventImpl>(func));
}

// Native C++ helper to securely map Mac48Address to a base Address object
Address ToAddress(const Mac48Address& mac) {
    return Address(mac);
}

}

using ns3::CreatePromiscCallback;
using ns3::SchedulePythonEvent;
using ns3::ToAddress;
""")

# ==============================================================================
# --- EXTERNAL HARDWARE MAPPINGS (UDP LATENCY CONTROLLER) ---
# ==============================================================================
# The external SSH IP addresses to reach each Pi
PI_CLUSTER = {
    1: "192.168.0.244",
    2: "192.168.0.198",
    3: "192.168.0.129",
    4: "192.168.0.237",
    5: "192.168.0.210",
    6: "192.168.0.201",
    7: "192.168.0.156",
    8: "192.168.0.222",
    9: "192.168.0.234",
    10: "192.168.0.165",
    11: "192.168.0.180",
}

# The internal VPN/VLAN IPs that `tc` runs against. 
TARGET_IPS = {
    1: "192.168.101.10",
    2: "192.168.102.10",
    3: "192.168.103.10",
    4: "192.168.104.10",
    5: "192.168.105.10",
    6: "192.168.106.10",
    7: "192.168.107.10",
    8: "192.168.108.10",
    9: "192.168.109.10",
    10: "192.168.110.10",
    11: "192.168.111.10"
}

UDP_PORT = 65000
PHYSICAL_BASELINE_OVERHEAD = 4

# Global socket for blasting latency configs to the physical hardware
g_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)


# --- Global Network Infrastructure ---
numNodes = 11

# Mesh Tracking Matrices (Used for instant O(1) reads during topology reporting)
meshDevices = [[None for _ in range(numNodes)] for _ in range(numNodes)]
linkBw = [[100.0 for _ in range(numNodes)] for _ in range(numNodes)]
linkDrop = [[0.0 for _ in range(numNodes)] for _ in range(numNodes)]
linkLatencies = [[0.0 for _ in range(numNodes)] for _ in range(numNodes)]

g_fdDev = [None for _ in range(numNodes)]

# Global MAC Address Translation table for distributed L2 switching
macToNodeMap = {}

# Reference Vault to prevent Python GC cleanup
g_keepAlive = []


# --- LIVE TOPOLOGY REPORTING ENGINE ---
def PrintCurrentTopology():
    print("\n=========================================================================")
    print(f" CORE TOPOLOGY REPORT | Sim Time: {ns.Simulator.Now().GetSeconds():.2f}s")
    print("=========================================================================")
    
    for i in range(numNodes):
        for j in range(i + 1, numNodes):
            if meshDevices[i][j] is None:
                continue
            
            # Read directly from the tracked YAML state matrices instead of ns-3 memory pointers
            bwMbps = linkBw[i][j]
            dropRate = linkDrop[i][j]
            latencyMs = linkLatencies[i][j]
                
            print(f"  Node {i + 1} <-> Node {j + 1} | Bandwidth: {bwMbps:>6.2f} Mbps | Packet Loss: {dropRate * 100.0:>4.1f}% | Latency: {latencyMs:>5.1f} ms")
            
    print("=========================================================================\n")


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

        if not srcMac.IsBroadcast():
            macToNodeMap[str(srcMac)] = myNodeID

        if dstMac.IsBroadcast() or dstMac.IsGroup():
            for i in range(numNodes):
                if i != myNodeID and meshDevices[myNodeID][i] is not None:
                    SendOverP2PTunnel(meshDevices[myNodeID][i], packet, protocol, src, dst)
        else: 
            dst_str = str(dstMac)
            if dst_str in macToNodeMap:
                targetNode = macToNodeMap[dst_str]
                if targetNode != myNodeID and meshDevices[myNodeID][targetNode] is not None:
                    SendOverP2PTunnel(meshDevices[myNodeID][targetNode], packet, protocol, src, dst)
            else:
                for i in range(numNodes):
                    if i != myNodeID and meshDevices[myNodeID][i] is not None:
                        SendOverP2PTunnel(meshDevices[myNodeID][i], packet, protocol, src, dst)
        return True
    return Ingress_From_Vlan


# --- 3. Split-Horizon Ingress (From Inter-Satellite Mesh Links) ---
def make_mesh_ingress_callback(myNodeID):
    def Ingress_From_Mesh(rxDevice, packet, protocol, src, dst, packetType):
        pktCopy = packet.Copy()
        eth = ns.EthernetHeader()
        pktCopy.RemoveHeader(eth)
        
        # Invokes the custom C++ bridge casting function to resolve the signature mapping crash
        g_fdDev[myNodeID].Send(pktCopy, cppyy.gbl.ToAddress(eth.GetDestination()), eth.GetLengthType())
        
        return True
    return Ingress_From_Mesh


# --- 4. Symmetric Dynamic Topology Engine ---
def ApplyLinkChanges(changes):
    # Pre-build the UDP JSON payloads to send to the Pis
    current_latency_config = {i: {} for i in PI_CLUSTER.keys()}

    for change in changes:
        src = change['src']
        dst = change['dst']
        
        if src < numNodes and dst < numNodes and src != dst:
            bw = 0.000001 if change['bwMbps'] <= 0.0 else change['bwMbps']
            drop = 1.0 if change['bwMbps'] <= 0.0 else change['dropRate']
            latency = change['latency']

            # --- A. Update the Global Tracking Matrices for the Printout ---
            linkBw[src][dst] = linkBw[dst][src] = bw
            linkDrop[src][dst] = linkDrop[dst][src] = drop
            linkLatencies[src][dst] = linkLatencies[dst][src] = latency

            # --- B. Enforce Bandwidth and Loss inside ns-3 ---
            newRate = ns.DataRate(f"{bw}Mbps")
            
            emDst = ns.CreateObject[ns.RateErrorModel]()
            emDst.SetAttribute("ErrorRate", ns.DoubleValue(drop))
            emDst.SetUnit(ns.RateErrorModel.ERROR_UNIT_PACKET)

            emSrc = ns.CreateObject[ns.RateErrorModel]()
            emSrc.SetAttribute("ErrorRate", ns.DoubleValue(drop))
            emSrc.SetUnit(ns.RateErrorModel.ERROR_UNIT_PACKET)

            meshDevices[src][dst].SetAttribute("DataRate", ns.DataRateValue(newRate))
            meshDevices[dst][src].SetAttribute("ReceiveErrorModel", ns.PointerValue(emDst))
            
            meshDevices[dst][src].SetAttribute("DataRate", ns.DataRateValue(newRate))
            meshDevices[src][dst].SetAttribute("ReceiveErrorModel", ns.PointerValue(emSrc))

            # --- C. Process Out-Of-Band Latency for the Pis ---
            # Translate 0-indexed ns-3 nodes back to 1-indexed Pi nodes (1, 2, 3, 4)
            pi_src = src + 1
            pi_dst = dst + 1
            
            adjusted_latency = max(0.0, latency - PHYSICAL_BASELINE_OVERHEAD)
            
            if pi_src in PI_CLUSTER and pi_dst in TARGET_IPS:
                current_latency_config[pi_src][TARGET_IPS[pi_dst]] = round(adjusted_latency, 2)
            if pi_dst in PI_CLUSTER and pi_src in TARGET_IPS:
                current_latency_config[pi_dst][TARGET_IPS[pi_src]] = round(adjusted_latency, 2)

    # --- D. Dispatch Latency Config via UDP Socket ---
    print("\n>> Out-Of-Band Latency Controller: Dispatching JSON payloads to Raspberry Pis...")
    for pi_id, target_map in current_latency_config.items():
        if not target_map:
            continue
            
        pi_ip = PI_CLUSTER[pi_id]
        try:
            json_payload = json.dumps(target_map).encode('utf-8')
            g_sock.sendto(json_payload, (pi_ip, UDP_PORT))
        except Exception as e:
            print(f"  -> Failed to send UDP latency config to Pi {pi_id} ({pi_ip}): {e}")

    PrintCurrentTopology()


def KeepAliveDummyEvent():
    pass


# --- 5. Dedicated YAML Parsing Engine ---
def parse_topology_trace(file_path):
    """
    Reads the YAML topology trace and structures it into a dictionary mapped by simulation time.
    """
    timelineMap = {}
    scheduleTime = 0.0
    inLink = False
    tempChange = {}

    try:
        with open(file_path, "r") as traceFile:
            for line in traceFile:
                line = line.strip()
                if not line or ":" not in line:
                    continue
                
                key, valStr = [x.strip() for x in line.split(":", 1)]

                if "- time" in key:
                    scheduleTime = float(valStr)
                elif "- src" in key:
                    # YAML is 1-indexed, Python arrays are 0-indexed
                    tempChange['src'] = int(valStr) - 1
                    inLink = True
                elif inLink and "dst" in key:
                    tempChange['dst'] = int(valStr) - 1
                elif inLink and "bw" in key:
                    tempChange['bwMbps'] = float(valStr)
                elif inLink and "drop" in key:
                    tempChange['dropRate'] = float(valStr)
                elif inLink and "latency" in key:
                    tempChange['latency'] = float(valStr)
                    
                    # Latency is the last element in the YAML block, trigger the append here
                    if scheduleTime not in timelineMap:
                        timelineMap[scheduleTime] = []
                    timelineMap[scheduleTime].append(tempChange.copy())
                    
                    inLink = False
                    tempChange = {}
    except FileNotFoundError:
        print(f"FATAL ERROR: Missing trajectory map at {file_path}!")
        sys.exit(1)
        
    return timelineMap


# --- 6. Real-Time Network Execution Environment ---
def main():
    ns.CommandLine().Parse(sys.argv)
    ns.GlobalValue.Bind("SimulatorImplementationType", ns.StringValue("ns3::RealtimeSimulatorImpl"))

    meshNodes = ns.NodeContainer()
    meshNodes.Create(numNodes)

    p2p = ns.PointToPointHelper()
    p2p.SetDeviceAttribute("DataRate", ns.StringValue("100Mbps"))
    # Native delay remains 0ms as latency is applied out-of-band on physical devices via 'tc'
    p2p.SetChannelAttribute("Delay", ns.StringValue("0ms"))
    p2p.SetDeviceAttribute("Mtu", ns.UintegerValue(1550))
    p2p.SetQueue("ns3::DropTailQueue<Packet>", "MaxSize", ns.QueueSizeValue(ns.QueueSize("5000p")))

    for i in range(numNodes):
        for j in range(i + 1, numNodes):
            linkNodes = ns.NodeContainer()
            linkNodes.Add(meshNodes.Get(i))
            linkNodes.Add(meshNodes.Get(j))
            
            devs = p2p.Install(linkNodes)
            devI = devs.Get(0)
            devJ = devs.Get(1)

            meshDevices[i][j] = devI
            meshDevices[j][i] = devJ

            cbI = make_mesh_ingress_callback(i)
            cbJ = make_mesh_ingress_callback(j)
            g_keepAlive.append(cbI)
            g_keepAlive.append(cbJ)

            devI.SetPromiscReceiveCallback(cppyy.gbl.CreatePromiscCallback(cbI))
            devJ.SetPromiscReceiveCallback(cppyy.gbl.CreatePromiscCallback(cbJ))

    emuHelper = ns.EmuFdNetDeviceHelper()
    emuHelper.SetAttribute("EncapsulationMode", ns.StringValue("Dix"))
    vlanMapping = ["vlan101", "vlan102", "vlan103", "vlan104","vlan105", "vlan106", "vlan107", "vlan108", "vlan109", "vlan110", "vlan111"]

    for i in range(numNodes):
        emuHelper.SetDeviceName(vlanMapping[i])
        devSide = emuHelper.Install(meshNodes.Get(i))
        g_fdDev[i] = devSide.Get(0)
        
        mac_addr = ns.Mac48Address.Allocate()
        g_fdDev[i].SetAttribute("Address", ns.Mac48AddressValue(mac_addr))

        cbVlan = make_vlan_ingress_callback(i)
        g_keepAlive.append(cbVlan)
        g_fdDev[i].SetPromiscReceiveCallback(cppyy.gbl.CreatePromiscCallback(cbVlan))

    # --- Call the standalone parser ---
    trace_file_path = "/home/ijoldenb/ns-3.48/scratch/topology_trace.yaml"
    timelineMap = parse_topology_trace(trace_file_path)

    # Schedule the link changes on the ns-3 timeline
    for t_time, changes in timelineMap.items():
        evt = lambda c=changes: ApplyLinkChanges(c)
        g_keepAlive.append(evt)
        ns.SchedulePythonEvent(ns.Seconds(t_time), evt)

    stopTime = ns.Seconds(3600.0)
    ns.Simulator.Stop(stopTime)
    ns.SchedulePythonEvent(stopTime - ns.Seconds(1.0), KeepAliveDummyEvent)

    print("=========================================================================")
    print(" ns-3 PointToPoint Mesh ACTIVE (BW/Loss) + Out-Of-Band UDP Latency Sync  ")
    print("=========================================================================")

    ns.Simulator.Run()
    ns.Simulator.Destroy()


if __name__ == '__main__':
    main()