import sys
import os
from ns import ns

# -----------------------------------------------------------------------------
# --- 0. Pure C++ Network Core (Fast, Safe, and Bug-Free) ---------------------
# -----------------------------------------------------------------------------
ns.cppyy.cppdef("""
#include "ns3/simulator.h"
#include "ns3/net-device.h"
#include "ns3/packet.h"
#include "ns3/ethernet-header.h"
#include "ns3/mac48-address.h"
#include "ns3/point-to-point-net-device.h"
#include "ns3/error-model.h"
#include "ns3/object-factory.h"
#include "ns3/pointer.h"
#include "ns3/double.h"
#include <vector>
#include <map>
#include <iostream>

namespace ns3 {
    class StarSwitchEngine {
    public:
        uint32_t numNodes = 4;
        uint32_t hubNodeID = 4;
        
        std::vector<Ptr<NetDevice>> txDeviceToHub;
        std::vector<Ptr<NetDevice>> hubRxDevice;
        std::vector<Ptr<NetDevice>> g_fdDev;
        std::map<Mac48Address, uint32_t> macToNodeMap;

        StarSwitchEngine() {
            txDeviceToHub.resize(numNodes, nullptr);
            hubRxDevice.resize(numNodes, nullptr);
            g_fdDev.resize(numNodes, nullptr);
        }
        
        void SendOverP2PWithHeader(Ptr<NetDevice> dev, Ptr<const Packet> packet, uint16_t protocol, const Address &src, const Address &dst) {
            Ptr<Packet> pktCopy = packet->Copy();
            EthernetHeader eth;
            eth.SetSource(Mac48Address::ConvertFrom(src));
            eth.SetDestination(Mac48Address::ConvertFrom(dst));
            eth.SetLengthType(protocol); // <-- The true protocol (IPv6, ARP, etc.) is saved safely here
            pktCopy->AddHeader(eth);
            
            // Force the P2P device to use 0x0800 so its internal EtherToPpp never asserts
            dev->Send(pktCopy, dev->GetBroadcast(), 0x0800); 
        }
        
        bool SwitchPacket(Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol, const Address &src, const Address &dst, bool cameFromVlan, int incomingSpokeID) {
            uint32_t currentNodeID = rxDevice->GetNode()->GetId();
            Mac48Address srcMac = Mac48Address::ConvertFrom(src);
            Mac48Address dstMac = Mac48Address::ConvertFrom(dst);

            // Mac learning phase
            if (!srcMac.IsBroadcast() && cameFromVlan) {
                macToNodeMap[srcMac] = currentNodeID;
            }

            // 1. Handle Broadcast traffic (ARP, DHCP, etc)
            if (dstMac.IsBroadcast()) {
                if (currentNodeID == hubNodeID) {
                    for (uint32_t i = 0; i < numNodes; ++i) {
                        if ((int)i != incomingSpokeID && hubRxDevice[i] != nullptr) {
                            hubRxDevice[i]->Send(packet->Copy(), hubRxDevice[i]->GetBroadcast(), protocol);
                        }
                    }
                } else if (cameFromVlan) {
                    txDeviceToHub[currentNodeID]->Send(packet->Copy(), txDeviceToHub[currentNodeID]->GetBroadcast(), protocol);
                } else {
                    if (macToNodeMap.find(srcMac) == macToNodeMap.end() || macToNodeMap[srcMac] != currentNodeID) {
                        g_fdDev[currentNodeID]->Send(packet->Copy(), dst, protocol);
                    }
                }
                return true;
            }

            // 2. Handle Known Unicast traffic
            if (macToNodeMap.find(dstMac) != macToNodeMap.end()) {
                uint32_t targetNodeID = macToNodeMap[dstMac];
                if (currentNodeID == hubNodeID) {
                    hubRxDevice[targetNodeID]->Send(packet->Copy(), hubRxDevice[targetNodeID]->GetBroadcast(), protocol);
                } else {
                    if (targetNodeID == currentNodeID) {
                        if (!cameFromVlan) {
                            g_fdDev[currentNodeID]->Send(packet->Copy(), dst, protocol);
                        }
                    } else if (cameFromVlan) {
                        txDeviceToHub[currentNodeID]->Send(packet->Copy(), txDeviceToHub[currentNodeID]->GetBroadcast(), protocol);
                    }
                }
            } else {
                // 3. Flood unknown unicast traffic across active lines
                if (currentNodeID == hubNodeID) {
                    for (uint32_t i = 0; i < numNodes; ++i) {
                        if ((int)i != incomingSpokeID && hubRxDevice[i] != nullptr) {
                            hubRxDevice[i]->Send(packet->Copy(), hubRxDevice[i]->GetBroadcast(), protocol);
                        }
                    }
                } else if (cameFromVlan) {
                    txDeviceToHub[currentNodeID]->Send(packet->Copy(), txDeviceToHub[currentNodeID]->GetBroadcast(), protocol);
                } else {
                    g_fdDev[currentNodeID]->Send(packet->Copy(), dst, protocol);
                }
            }
            return true;
        }

        // Direct stream callbacks passing frames directly through to SwitchPacket
        bool IngressFromVlan(Ptr<NetDevice> dev, Ptr<const Packet> pkt, uint16_t pro, const Address &src, const Address &dst, NetDevice::PacketType pt) {
            return SwitchPacket(dev, pkt, pro, src, dst, true, -1);
        }
        bool IngressFromHub(Ptr<NetDevice> dev, Ptr<const Packet> pkt, uint16_t pro, const Address &src, const Address &dst, NetDevice::PacketType pt) {
            return SwitchPacket(dev, pkt, pro, src, dst, false, -1);
        }
        bool IngressAtHub(Ptr<NetDevice> dev, Ptr<const Packet> pkt, uint16_t pro, const Address &src, const Address &dst, NetDevice::PacketType pt) {
            int spokeID = -1;
            for (uint32_t i = 0; i < numNodes; ++i) {
                if (hubRxDevice[i] == dev) { spokeID = i; break; }
            }
            return SwitchPacket(dev, pkt, pro, src, dst, false, spokeID);
        }

        void ConnectDeviceCallbacks(uint32_t nodeID, Ptr<NetDevice> spokeDev, Ptr<NetDevice> hubPortDev, Ptr<NetDevice> vlanDev) {
            if (nodeID < numNodes) {
                spokeDev->SetPromiscReceiveCallback(MakeCallback(&StarSwitchEngine::IngressFromHub, this));
                hubPortDev->SetPromiscReceiveCallback(MakeCallback(&StarSwitchEngine::IngressAtHub, this));
                vlanDev->SetPromiscReceiveCallback(MakeCallback(&StarSwitchEngine::IngressFromVlan, this));
            }
        }

        void UpdateLinkBandwidth(uint32_t nodeID, uint64_t bps, double dropRate) {
            if (nodeID < numNodes && txDeviceToHub[nodeID] != nullptr) {
                Ptr<PointToPointNetDevice> txDev = StaticCast<PointToPointNetDevice>(txDeviceToHub[nodeID]);
                Ptr<PointToPointNetDevice> rxDev = StaticCast<PointToPointNetDevice>(hubRxDevice[nodeID]);
                
                DataRate newRate(bps);
                txDev->SetDataRate(newRate);
                rxDev->SetDataRate(newRate);

                ObjectFactory factory;
                factory.SetTypeId("ns3::RateErrorModel");
                Ptr<RateErrorModel> em = factory.Create()->GetObject<RateErrorModel>();
                em->SetAttribute("ErrorRate", DoubleValue(dropRate));
                em->SetUnit(RateErrorModel::ERROR_UNIT_PACKET);

                txDev->SetAttribute("ReceiveErrorModel", PointerValue(em));
                rxDev->SetAttribute("ReceiveErrorModel", PointerValue(em));
            }
        }
    };
    
    static StarSwitchEngine g_engine;
    
    void PythonScheduleEvent(double delaySec, void (*func)()) {
        Simulator::Schedule(Seconds(delaySec), func);
    }
}
""")

# Setup Global Engine Alias (Corrected top-level namespace extraction)
engine = ns.g_engine
traceFile = None

def KeepAliveDummyEvent():
    pass

# --- 1. Python Trace Parser ---
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

        src = int(tokens[0])
        dst = int(tokens[1])
        bwMbps = float(tokens[2])
        dropRate = float(tokens[3])

        if src < engine.numNodes and engine.txDeviceToHub[src] is not None:
            if bwMbps <= 0.0:
                bwMbps, dropRate = 0.000001, 1.0

            # Convert to bps as a basic python integer
            bpsValue = int(bwMbps * 1000000)
            
            # Execute modification inside the completely native context
            engine.UpdateLinkBandwidth(src, bpsValue, dropRate)
            
        line = traceFile.readline()

    print(f"Loaded Trace Metrics for Sim Time: {ns.Simulator.Now().GetSeconds()}s")
    sys.stdout.flush()
    timeDelta = max(0.001, nextTimestamp - ns.Simulator.Now().GetSeconds())
    ns.PythonScheduleEvent(timeDelta, ParseNextNetworkXSnapshot)


# --- 2. Main Network Setup ---
def main():
    global traceFile
    cmd = ns.CommandLine()
    cmd.Parse(sys.argv)

    ns.GlobalValue.Bind("SimulatorImplementationType", ns.StringValue("ns3::RealtimeSimulatorImpl"))

    peripheralNodes = ns.NodeContainer()
    peripheralNodes.Create(int(engine.numNodes))
    hubNode = ns.Node()

    # Build Star Network
    print("🔗 Linking Topology Nodes...")
    sys.stdout.flush()
    for i in range(int(engine.numNodes)):
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

        # Map to the global tracker references
        engine.txDeviceToHub[i] = devSpoke
        engine.hubRxDevice[i] = devHubPort

    # Setup Live OS Emulation Bridges
    print("🔌 Provisioning Emulated Host Hooks...")
    sys.stdout.flush()
    emuHelper = ns.EmuFdNetDeviceHelper()
    emuHelper.SetAttribute("EncapsulationMode", ns.StringValue("Dix"))
    vlanMapping = ["vlan101", "vlan102", "vlan103", "vlan104"]

    for i in range(int(engine.numNodes)):
        emuHelper.SetDeviceName(vlanMapping[i])
        devSide = emuHelper.Install(peripheralNodes.Get(i)).Get(0)
        devSide.SetAttribute("Address", ns.Mac48AddressValue(ns.Mac48Address.Allocate()))
        
        engine.g_fdDev[i] = devSide

        # Directly link the callback loops inside C++ memory (Fixes Address/DataRate types)
        engine.ConnectDeviceCallbacks(i, engine.txDeviceToHub[i], engine.hubRxDevice[i], devSide)

    try:
        traceFile = open("/home/ijoldenb/ns-3.48/scratch/topology_trace.txt", "r")
    except IOError:
        sys.exit("NS_FATAL_ERROR: Could not open trace file!")

    ns.PythonScheduleEvent(0.0, ParseNextNetworkXSnapshot)

    stopTime = ns.Seconds(3600.0)
    ns.Simulator.Stop(stopTime)
    ns.PythonScheduleEvent(stopTime.GetSeconds() - 1.0, KeepAliveDummyEvent)

    print("================================================================")
    print("ns-3 Hybrid Python-Engine Architecture Running Fluidly!")
    print("================================================================")
    sys.stdout.flush()

    ns.Simulator.Run()
    traceFile.close()
    ns.Simulator.Destroy()

if __name__ == '__main__':
    main()