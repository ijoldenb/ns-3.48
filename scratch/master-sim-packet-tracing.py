import sys
import os
import socket
import json
import csv
import time
import threading
import yaml
from collections import defaultdict
from ns import ns
import cppyy

# ==============================================================================
# --- 0. PATHS & GLOBAL CONFIGURATION ---
# ==============================================================================
laptopPath = os.path.expanduser("~/RoutingScripts/")
CSV_FILENAME = os.path.expanduser("~/RoutingScripts/Data/network_data.csv")
TRACE_FILE_PATH = os.path.expanduser("~/ns-3.48/scratch/topology_trace.yaml")

TELEMETRY_PORT = 65001
UDP_PORT = 65000
PHYSICAL_BASELINE_OVERHEAD = 4

# Global socket for blasting out-of-band latency configs to physical Pis
g_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

# ==============================================================================
# --- 1. CRITICAL C++ TO PYTHON CALLBACK BRIDGE & CASTING ENGINE ---
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

Address ToAddress(const Mac48Address& mac) {
    return Address(mac);
}

}

using ns3::CreatePromiscCallback;
using ns3::SchedulePythonEvent;
using ns3::ToAddress;
""")

# ==============================================================================
# --- 2. BACKGROUND TELEMETRY TRACE COLLECTOR ---
# ==============================================================================
def start_telemetry_collector():
    """Runs in a background thread to collect RTT data over UDP port 65001."""
    os.makedirs(os.path.dirname(CSV_FILENAME), exist_ok=True)
    
    trace_buffer = defaultdict(dict)
    latest_legs = {}

    with open(CSV_FILENAME, mode='w', newline='') as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(['Timestamp', 'Sender', 'Receiver', 'Two_Way_Latency_ms'])

    telemetry_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    telemetry_sock.bind(("0.0.0.0", TELEMETRY_PORT))

    print(f"[*] Central Trace Collector active on UDP {TELEMETRY_PORT}...")
    print(f"[*] Writing 2-way RTT telemetry data to '{CSV_FILENAME}'...\n")

    with open(CSV_FILENAME, mode='a', newline='') as csv_file:
        writer = csv.writer(csv_file)
        while True:
            try:
                data, _ = telemetry_sock.recvfrom(1024)
                msg = json.loads(data.decode('utf-8'))
                
                packet_key = (msg["ip_id"], msg["src"], msg["dst"])
                direction = msg["dir"]
                timestamp = msg["ts"]
                
                trace_buffer[packet_key][direction] = timestamp
                
                if "tx" in trace_buffer[packet_key] and "rx" in trace_buffer[packet_key]:
                    tx_time = trace_buffer[packet_key]["tx"]
                    rx_time = trace_buffer[packet_key]["rx"]
                    
                    one_way_ms = (rx_time - tx_time) * 1000.0
                    src = msg["src"]
                    dst = msg["dst"]
                    
                    latest_legs[(src, dst)] = one_way_ms
                    
                    if (dst, src) in latest_legs:
                        two_way_ms = latest_legs[(src, dst)] + latest_legs[(dst, src)]
                        current_time = time.strftime("%Y-%m-%d %H:%M:%S")
                        
                        sender = dst
                        receiver = src
                        
                        print(f"[RTT] {sender} -> {receiver} | 2-Way Latency: {two_way_ms:.3f} ms")
                        writer.writerow([current_time, sender, receiver, f"{two_way_ms:.3f}"])
                        csv_file.flush() 
                        
                        del latest_legs[(src, dst)]
                        del latest_legs[(dst, src)]
                        
                    del trace_buffer[packet_key]

            except json.JSONDecodeError:
                continue
            except Exception as e:
                print(f"[!] Telemetry Collector exception: {e}")
                break

# ==============================================================================
# --- 3. DYNAMIC CONFIGURATION & IP MAPPINGS ---
# ==============================================================================
def load_ip_config(file_path):
    expanded_path = os.path.expanduser(file_path)
    with open(expanded_path) as f:
        data = yaml.safe_load(f)
    if isinstance(next(iter(data.values())), dict):
        data = next(iter(data.values()))
    return {int(''.join(filter(str.isdigit, str(k)))): str(v).strip() for k, v in data.items()}

# Load IPs dynamically
PI_CLUSTER = load_ip_config(os.path.join(laptopPath, "control_IP.yaml"))
print(">> Loaded Control IPs:")
for pi_id, ip in sorted(PI_CLUSTER.items()):
    print(f"   Pi #{pi_id} -> {ip}")

TARGET_IPS = load_ip_config(os.path.join(laptopPath, "sim_IP.yaml"))
print(">> Loaded Sim IPs:")
for pi_id, ip in sorted(TARGET_IPS.items()):
    print(f"   Pi #{pi_id} -> {ip}")

numNodes = len(TARGET_IPS)

# Mesh Tracking Matrices
meshDevices = [[None for _ in range(numNodes)] for _ in range(numNodes)]
linkBw = [[100.0 for _ in range(numNodes)] for _ in range(numNodes)]
linkDrop = [[0.0 for _ in range(numNodes)] for _ in range(numNodes)]
linkLatencies = [[0.0 for _ in range(numNodes)] for _ in range(numNodes)]

g_fdDev = [None for _ in range(numNodes)]
macToNodeMap = {}
g_keepAlive = []

# ==============================================================================
# --- 4. NS-3 SWITCHING & TOPOLOGY ENGINE ---
# ==============================================================================
def PrintCurrentTopology():
    print("\n=========================================================================")
    print(f" CORE TOPOLOGY REPORT | Sim Time: {ns.Simulator.Now().GetSeconds():.2f}s")
    print("=========================================================================")
    
    for i in range(numNodes):
        for j in range(i + 1, numNodes):
            if meshDevices[i][j] is None:
                continue
            
            bwMbps = linkBw[i][j]
            dropRate = linkDrop[i][j]
            latencyMs = linkLatencies[i][j]
                
            print(f"  Node {i + 1} <-> Node {j + 1} | Bandwidth: {bwMbps:>6.2f} Mbps | Packet Loss: {dropRate * 100.0:>4.1f}% | Latency: {latencyMs:>5.1f} ms")
            
    print("=========================================================================\n")


def SendOverP2PTunnel(dev, packet, protocol, src_addr, dst_addr):
    pktCopy = packet.Copy()
    eth = ns.EthernetHeader()
    eth.SetSource(ns.Mac48Address.ConvertFrom(src_addr))
    eth.SetDestination(ns.Mac48Address.ConvertFrom(dst_addr))
    eth.SetLengthType(protocol)
    
    pktCopy.AddHeader(eth)
    dev.Send(pktCopy, dev.GetBroadcast(), 0x0800)


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


def make_mesh_ingress_callback(myNodeID):
    def Ingress_From_Mesh(rxDevice, packet, protocol, src, dst, packetType):
        pktCopy = packet.Copy()
        eth = ns.EthernetHeader()
        pktCopy.RemoveHeader(eth)
        
        g_fdDev[myNodeID].Send(pktCopy, cppyy.gbl.ToAddress(eth.GetDestination()), eth.GetLengthType())
        return True
    return Ingress_From_Mesh


def ApplyLinkChanges(changes):
    current_latency_config = {i: {} for i in PI_CLUSTER.keys()}

    for change in changes:
        src = change['src']
        dst = change['dst']
        
        if src < numNodes and dst < numNodes and src != dst:
            bw = 0.000001 if change['bwMbps'] <= 0.0 else change['bwMbps']
            drop = 1.0 if change['bwMbps'] <= 0.0 else change['dropRate']
            latency = change['latency']

            linkBw[src][dst] = linkBw[dst][src] = bw
            linkDrop[src][dst] = linkDrop[dst][src] = drop
            linkLatencies[src][dst] = linkLatencies[dst][src] = latency

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

            pi_src = src + 1
            pi_dst = dst + 1
            
            adjusted_latency = max(0.0, latency - PHYSICAL_BASELINE_OVERHEAD)
            
            if pi_src in PI_CLUSTER and pi_dst in TARGET_IPS:
                current_latency_config[pi_src][TARGET_IPS[pi_dst]] = round(adjusted_latency, 2)
            if pi_dst in PI_CLUSTER and pi_src in TARGET_IPS:
                current_latency_config[pi_dst][TARGET_IPS[pi_src]] = round(adjusted_latency, 2)

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


def parse_topology_trace(file_path):
    expanded_path = os.path.expanduser(file_path)
    timelineMap = {}
    scheduleTime = 0.0
    inLink = False
    tempChange = {}

    try:
        with open(expanded_path, "r") as traceFile:
            for line in traceFile:
                line = line.strip()
                if not line or ":" not in line:
                    continue
                
                key, valStr = [x.strip() for x in line.split(":", 1)]

                if "- time" in key:
                    scheduleTime = float(valStr)
                elif "- src" in key:
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
                    
                    if scheduleTime not in timelineMap:
                        timelineMap[scheduleTime] = []
                    timelineMap[scheduleTime].append(tempChange.copy())
                    
                    inLink = False
                    tempChange = {}
    except FileNotFoundError:
        print(f"FATAL ERROR: Missing trajectory map at {expanded_path}!")
        sys.exit(1)
        
    return timelineMap


# ==============================================================================
# --- 5. MAIN EXECUTION ENVIRONMENT ---
# ==============================================================================
def main():
    # 1. Spawn Telemetry Collector as a background daemon thread
    collector_thread = threading.Thread(target=start_telemetry_collector, daemon=True)
    collector_thread.start()

    # 2. Configure ns-3 Realtime Simulator
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
            g_keepAlive.extend([cbI, cbJ])

            devI.SetPromiscReceiveCallback(cppyy.gbl.CreatePromiscCallback(cbI))
            devJ.SetPromiscReceiveCallback(cppyy.gbl.CreatePromiscCallback(cbJ))

    emuHelper = ns.EmuFdNetDeviceHelper()
    emuHelper.SetAttribute("EncapsulationMode", ns.StringValue("Dix"))
    vlanMapping = [f"vlan{101 + i}" for i in range(numNodes)]

    for i in range(numNodes):
        emuHelper.SetDeviceName(vlanMapping[i])
        devSide = emuHelper.Install(meshNodes.Get(i))
        g_fdDev[i] = devSide.Get(0)
        
        mac_addr = ns.Mac48Address.Allocate()
        g_fdDev[i].SetAttribute("Address", ns.Mac48AddressValue(mac_addr))

        cbVlan = make_vlan_ingress_callback(i)
        g_keepAlive.append(cbVlan)
        g_fdDev[i].SetPromiscReceiveCallback(cppyy.gbl.CreatePromiscCallback(cbVlan))

    # Parse trajectory timeline
    timelineMap = parse_topology_trace(TRACE_FILE_PATH)

    # Schedule changes
    for t_time, changes in timelineMap.items():
        evt = lambda c=changes: ApplyLinkChanges(c)
        g_keepAlive.append(evt)
        ns.SchedulePythonEvent(ns.Seconds(t_time), evt)

    stopTime = ns.Seconds(3600.0)
    ns.Simulator.Stop(stopTime)
    ns.SchedulePythonEvent(stopTime - ns.Seconds(1.0), lambda: None)

    print("=========================================================================")
    print(" ns-3 PointToPoint Mesh ACTIVE (BW/Loss) + Out-Of-Band Latency Sync  ")
    print("=========================================================================")

    ns.Simulator.Run()
    ns.Simulator.Destroy()


if __name__ == '__main__':
    main()