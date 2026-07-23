import sys
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

# --- Global Network Infrastructure ---
numNodes = 4

# Mesh Tracking Matrix: meshDevices[SrcNode][DstNode]
meshDevices = [[None for _ in range(numNodes)] for _ in range(numNodes)]
g_fdDev = [None for _ in range(numNodes)]

# Global MAC Address Translation table for distributed L2 switching
macToNodeMap = {}

# Reference Vault to prevent Python GC cleanup
g_keepAlive = []


# --- LIVE TOPOLOGY REPORTING ENGINE ---
def PrintCurrentTopology():
    print("\n====================================================")
    print(f" CORE TOPOLOGY REPORT | Sim Time: {ns.Simulator.Now().GetSeconds()}s")
    print("====================================================")
    
    for i in range(numNodes):
        for j in range(i + 1, numNodes):
            if meshDevices[i][j] is None:
                continue
            
            # 1. Fetch data rate attribute directly from the active PointToPoint device
            bpsVal = ns.DataRateValue()
            meshDevices[i][j].GetAttribute("DataRate", bpsVal)
            bwMbps = bpsVal.Get().GetBitRate() / 1000000.0
            
            # 2. Extract the Receive Error Model via the dynamic pointer attribute wrapper
            ptrVal = ns.PointerValue()
            meshDevices[j][i].GetAttribute("ReceiveErrorModel", ptrVal)
            em = ptrVal.GetObject()  
            
            dropRate = 0.0
            if em:
                dropVal = ns.DoubleValue()
                em.GetAttribute("ErrorRate", dropVal)
                dropRate = dropVal.Get()
                
            print(f"  Node {i + 1} <-> Node {j + 1} | Bandwidth: {bwMbps:.2f} Mbps | Packet Loss: {dropRate * 100.0:.1f}%")
            
    print("====================================================\n")


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
    for change in changes:
        src = change['src']
        dst = change['dst']
        
        if src < numNodes and dst < numNodes and src != dst:
            bw = 0.000001 if change['bwMbps'] <= 0.0 else change['bwMbps']
            drop = 1.0 if change['bwMbps'] <= 0.0 else change['dropRate']

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
    vlanMapping = ["vlan101", "vlan102", "vlan103", "vlan104"]

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

    # Schedule the link changes
    for t_time, changes in timelineMap.items():
        evt = lambda c=changes: ApplyLinkChanges(c)
        g_keepAlive.append(evt)
        ns.SchedulePythonEvent(ns.Seconds(t_time), evt)

    stopTime = ns.Seconds(3600.0)
    ns.Simulator.Stop(stopTime)
    ns.SchedulePythonEvent(stopTime - ns.Seconds(1.0), KeepAliveDummyEvent)

    print("================================================================")
    print("ns-3 Dynamic PointToPoint FULL MESH Active (Python L2 Split-Horizon)")
    print("================================================================")

    ns.Simulator.Run()
    ns.Simulator.Destroy()


if __name__ == '__main__':
    main()