
#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/error-model.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <map>

using namespace ns3;

void KeepAliveDummyEvent () {}

constexpr int numNodes = 4;   // 4 Peripheral nodes connected to 4 physical Pis
constexpr int hubNodeID = 4;  // Internal 5th node acting as the star hub

// Tracking matrices for spoke-to-hub connections
Ptr<CsmaChannel> channelToHub[numNodes];
Ptr<CsmaNetDevice> txDeviceToHub[numNodes]; // Peripheral node side transmitting to Hub
Ptr<CsmaNetDevice> hubRxDevice[numNodes];   // Hub side receiving from specific peripheral
Ptr<NetDevice> g_fdDev[numNodes];

// Centralized MAC table tracking which peripheral Node ID owns which physical Pi MAC
std::map<Mac48Address, uint32_t> macToNodeMap;

// --- 1. Star Network Core Switching Engine ---
void SwitchPacket (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                    const Address &src, const Address &dst, bool cameFromVlan, int incomingSpokeID)
{
  uint32_t currentNodeID = rxDevice->GetNode()->GetId();
  Mac48Address srcMac = Mac48Address::ConvertFrom(src);
  Mac48Address dstMac = Mac48Address::ConvertFrom(dst);

  // Dynamic MAC Learning
  if (!srcMac.IsBroadcast() && cameFromVlan)
    {
      macToNodeMap[srcMac] = currentNodeID;
    }

  // Handle Broadcast Traffic (ARP)
  if (dstMac.IsBroadcast())
    {
      if (currentNodeID == hubNodeID)
        {
          for (int i = 0; i < numNodes; ++i)
            {
              if (i != incomingSpokeID && txDeviceToHub[i] != nullptr)
                {
                  Ptr<Packet> pktCopy = packet->Copy();
                  hubRxDevice[i]->SendFrom(pktCopy, src, dst, protocol);
                }
            }
        }
      else if (cameFromVlan)
        {
          Ptr<Packet> pktCopy = packet->Copy();
          txDeviceToHub[currentNodeID]->SendFrom(pktCopy, src, dst, protocol);
        }
      else
        {
          Ptr<Packet> pktCopy = packet->Copy();
          g_fdDev[currentNodeID]->Send(pktCopy, dst, protocol);
        }
      return;
    }

  // Handle Unicast Traffic (IP Packets)
  if (macToNodeMap.find(dstMac) != macToNodeMap.end())
    {
      uint32_t targetNodeID = macToNodeMap[dstMac];

      if (currentNodeID == hubNodeID)
        {
          Ptr<Packet> pktCopy = packet->Copy();
          hubRxDevice[targetNodeID]->SendFrom(pktCopy, src, dst, protocol);
        }
      else
        {
          if (targetNodeID == currentNodeID)
            {
              if (!cameFromVlan)
                {
                  Ptr<Packet> pktCopy = packet->Copy();
                  g_fdDev[currentNodeID]->Send(pktCopy, dst, protocol);
                }
            }
          else if (cameFromVlan)
            {
              Ptr<Packet> pktCopy = packet->Copy();
              txDeviceToHub[currentNodeID]->SendFrom(pktCopy, src, dst, protocol);
            }
        }
    }
  else
    {
      // Unknown Unicast Fallback: Flood like a broadcast frame
      if (currentNodeID == hubNodeID)
        {
          for (int i = 0; i < numNodes; ++i)
            {
              if (i != incomingSpokeID)
                {
                  Ptr<Packet> pktCopy = packet->Copy();
                  hubRxDevice[i]->SendFrom(pktCopy, src, dst, protocol);
                }
            }
        }
      else if (cameFromVlan)
        {
          Ptr<Packet> pktCopy = packet->Copy();
          txDeviceToHub[currentNodeID]->SendFrom(pktCopy, src, dst, protocol);
        }
      else
        {
          Ptr<Packet> pktCopy = packet->Copy();
          g_fdDev[currentNodeID]->Send(pktCopy, dst, protocol);
        }
    }
}

// --- 2. Clean Callback Interfaces (Matching ns-3 Promisc Signatures Exactly) ---

bool Ingress_From_Vlan (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                         const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  SwitchPacket(rxDevice, packet, protocol, src, dst, true, -1);
  return true;
}

bool Ingress_From_Hub (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                        const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  SwitchPacket(rxDevice, packet, protocol, src, dst, false, -1);
  return true;
}

bool Ingress_At_Hub (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                      const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  int spokeID = -1;
  for (int i = 0; i < numNodes; ++i)
    {
      if (hubRxDevice[i] == rxDevice)
        {
          spokeID = i;
          break;
        }
    }
  SwitchPacket(rxDevice, packet, protocol, src, dst, false, spokeID);
  return true;
}

// --- 3. Dynamic NetworkX Trace Parser ---
void ParseNextNetworkXSnapshot (std::shared_ptr<std::ifstream> fileStream)
{
  if (fileStream->eof())
    {
      NS_LOG_UNCOND ("End of topology trace file reached.");
      return;
    }

  std::string line, token;
  double nextTimestamp = 0.0;

  // Find the next TIMESTAMP block marker
  while (std::getline (*fileStream, line))
    {
      std::stringstream ss (line);
      ss >> token;
      if (token == "TIMESTAMP")
        {
          ss >> nextTimestamp;
          break;
        }
    }

  // Process metrics line by line until END marker
  while (std::getline (*fileStream, line))
    {
      if (line.empty() || line[0] == '#') continue; 
      
      std::stringstream ss (line);
      ss >> token;
      if (token == "END") break; 

      ss.clear();
      ss.str (line);
      
      uint32_t src, dst;
      double bwMbps, dropRate;
      ss >> src >> dst >> bwMbps >> dropRate;

      // In a star network, the characteristics of a communication between 'src' and the network 
      // map directly onto the spoke belonging to that peripheral node ID.
      if (src < numNodes && channelToHub[src] != nullptr)
        {
          // Update spoke bandwidth dynamically (0 Mbps cuts off the link entirely)
          channelToHub[src]->SetAttribute ("DataRate", DataRateValue (DataRate (bwMbps * 1000000)));
          
          // Update spoke propagation latency dynamically
          channelToHub[src]->SetAttribute ("Delay", TimeValue (MilliSeconds (0)));

          // Dynamically adjust packet loss rules for this spoke interface
          Ptr<RateErrorModel> em = CreateObject<RateErrorModel> ();
          em->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          em->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          
          // Attach loss models symmetrically to both sides of the hub connection
          txDeviceToHub[src]->SetAttribute ("ReceiveErrorModel", PointerValue (em));
          hubRxDevice[src]->SetAttribute ("ReceiveErrorModel", PointerValue (em));
        }
    }

  NS_LOG_UNCOND ("Loaded NetworkX Trace Metrics for Sim Time: " << Simulator::Now ().GetSeconds () << "s");

  // Schedule the next parser cycle execution
  double currentTime = Simulator::Now ().GetSeconds ();
  double timeDelta = nextTimestamp - currentTime;
  
  if (timeDelta > 0)
    {
      Simulator::Schedule (Seconds (timeDelta), &ParseNextNetworkXSnapshot, fileStream);
    }
  else
    {
      Simulator::Schedule (MilliSeconds (1), &ParseNextNetworkXSnapshot, fileStream);
    }
}

// --- 4. Main Network Setup ---
int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  NodeContainer peripheralNodes;
  peripheralNodes.Create (numNodes);

  Ptr<Node> hubNode = CreateObject<Node> ();

  // Build Star Spokes
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      CsmaHelper csma;
      csma.SetChannelAttribute ("DataRate", StringValue ("100Mbps")); // Initial speed

      NodeContainer linkNodes (peripheralNodes.Get (i), hubNode);
      NetDeviceContainer devs = csma.Install (linkNodes);

      Ptr<CsmaNetDevice> devSpoke = DynamicCast<CsmaNetDevice> (devs.Get (0));
      Ptr<CsmaNetDevice> devHubPort = DynamicCast<CsmaNetDevice> (devs.Get (1));
      Ptr<CsmaChannel> ch = DynamicCast<CsmaChannel> (devSpoke->GetChannel ());

      channelToHub[i] = ch;
      txDeviceToHub[i] = devSpoke;
      hubRxDevice[i] = devHubPort; 

      // Connect Spoke and Hub port pipelines
      devSpoke->SetPromiscReceiveCallback (MakeCallback (&Ingress_From_Hub));
      devHubPort->SetPromiscReceiveCallback (MakeCallback (&Ingress_At_Hub));
    }

  // Initialize Raw OS File Descriptor Portals
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));

  std::vector<std::string> vlanMapping = {"vlan101", "vlan102", "vlan103", "vlan104"};

  for (int i = 0; i < numNodes; ++i)
    {
      emuHelper.SetDeviceName (vlanMapping[i]);
      
      NetDeviceContainer devSide = emuHelper.Install (peripheralNodes.Get(i));
      g_fdDev[i] = devSide.Get(0);
      g_fdDev[i]->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

      g_fdDev[i]->SetPromiscReceiveCallback (MakeCallback (&Ingress_From_Vlan));
    }

  // --- Start the Topology Trace File Reader ---
  auto traceFile = std::make_shared<std::ifstream> ("/home/ijoldenb/ns-3.48/scratch/topology_trace.txt");
  if (!traceFile->is_open ())
    {
      NS_FATAL_ERROR ("Could not open topology_trace.txt!");
    }
  
  Simulator::Schedule (Seconds (0.0), &ParseNextNetworkXSnapshot, traceFile);

  Time stopTime = Seconds (3600.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 Dynamic Star-Topology Trace Emulation Active");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}