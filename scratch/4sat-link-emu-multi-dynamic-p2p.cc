#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h" // Shifted to P2P
#include "ns3/fd-net-device-module.h"
#include "ns3/error-model.h"
#include "ns3/ethernet-header.h"       // Required for our custom MAC encapsulation
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <map>

using namespace ns3;

void KeepAliveDummyEvent () {}

constexpr int numNodes = 4;   
constexpr int hubNodeID = 4;  

// Tracking matrices for P2P spoke-to-hub connections
Ptr<PointToPointChannel> channelToHub[numNodes];
Ptr<PointToPointNetDevice> txDeviceToHub[numNodes]; 
Ptr<PointToPointNetDevice> hubRxDevice[numNodes];   
Ptr<NetDevice> g_fdDev[numNodes];

std::map<Mac48Address, uint32_t> macToNodeMap;

// --- 1. Custom Encapsulation Tunnel for P2P ---
// P2P links don't natively carry MAC addresses. This forces the original 
// VLAN MAC addresses into a header so they survive the trip to the virtual Hub.
void SendOverP2PTunnel (Ptr<NetDevice> dev, Ptr<const Packet> packet, uint16_t protocol, const Address &src, const Address &dst)
{
  Ptr<Packet> pktCopy = packet->Copy();
  EthernetHeader eth;
  eth.SetSource (Mac48Address::ConvertFrom (src));
  eth.SetDestination (Mac48Address::ConvertFrom (dst));
  eth.SetLengthType (protocol);
  
  pktCopy->AddHeader (eth);
  dev->Send (pktCopy, dev->GetBroadcast (), 0x0800); // Send across the P2P wire
}


// --- 2. Star Network Core Switching Engine ---
void SwitchPacket (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                    const Address &src, const Address &dst, bool cameFromVlan, int incomingSpokeID)
{
  uint32_t currentNodeID = rxDevice->GetNode()->GetId();
  Mac48Address srcMac = Mac48Address::ConvertFrom(src);
  Mac48Address dstMac = Mac48Address::ConvertFrom(dst);

  if (!srcMac.IsBroadcast() && cameFromVlan)
    {
      macToNodeMap[srcMac] = currentNodeID;
    }

  if (dstMac.IsBroadcast())
    {
      if (currentNodeID == hubNodeID)
        {
          for (int i = 0; i < numNodes; ++i)
            {
              if (i != incomingSpokeID && txDeviceToHub[i] != nullptr)
                {
                  SendOverP2PTunnel (hubRxDevice[i], packet, protocol, src, dst);
                }
            }
        }
      else if (cameFromVlan)
        {
          SendOverP2PTunnel (txDeviceToHub[currentNodeID], packet, protocol, src, dst);
        }
      else
        {
          Ptr<Packet> pktCopy = packet->Copy();
          g_fdDev[currentNodeID]->Send(pktCopy, dst, protocol);
        }
      return;
    }

  if (macToNodeMap.find(dstMac) != macToNodeMap.end())
    {
      uint32_t targetNodeID = macToNodeMap[dstMac];

      if (currentNodeID == hubNodeID)
        {
          SendOverP2PTunnel (hubRxDevice[targetNodeID], packet, protocol, src, dst);
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
              SendOverP2PTunnel (txDeviceToHub[currentNodeID], packet, protocol, src, dst);
            }
        }
    }
  else
    {
      if (currentNodeID == hubNodeID)
        {
          for (int i = 0; i < numNodes; ++i)
            {
              if (i != incomingSpokeID)
                {
                  SendOverP2PTunnel (hubRxDevice[i], packet, protocol, src, dst);
                }
            }
        }
      else if (cameFromVlan)
        {
          SendOverP2PTunnel (txDeviceToHub[currentNodeID], packet, protocol, src, dst);
        }
      else
        {
          Ptr<Packet> pktCopy = packet->Copy();
          g_fdDev[currentNodeID]->Send(pktCopy, dst, protocol);
        }
    }
}

// --- 3. Clean Callback Interfaces ---

bool Ingress_From_Vlan (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                         const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  SwitchPacket(rxDevice, packet, protocol, src, dst, true, -1);
  return true;
}

// Decapsulates the MAC address before hitting the switch loop
bool Ingress_From_Hub (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                        const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy();
  EthernetHeader eth;
  pktCopy->RemoveHeader (eth);
  
  SwitchPacket (rxDevice, pktCopy, eth.GetLengthType(), eth.GetSource(), eth.GetDestination(), false, -1);
  return true;
}

// Decapsulates the MAC address before hitting the switch loop
bool Ingress_At_Hub (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                      const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy();
  EthernetHeader eth;
  pktCopy->RemoveHeader (eth);

  int spokeID = -1;
  for (int i = 0; i < numNodes; ++i)
    {
      if (hubRxDevice[i] == rxDevice)
        {
          spokeID = i;
          break;
        }
    }
  SwitchPacket (rxDevice, pktCopy, eth.GetLengthType(), eth.GetSource(), eth.GetDestination(), false, spokeID);
  return true;
}

// --- 4. Dynamic NetworkX Trace Parser ---
void ParseNextNetworkXSnapshot (std::shared_ptr<std::ifstream> fileStream)
{
  if (fileStream->eof()) return;

  std::string line, token;
  double nextTimestamp = 0.0;

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

      if (src < numNodes && channelToHub[src] != nullptr)
        {
          if (bwMbps <= 0.0) 
            {
              bwMbps = 0.000001; 
              dropRate = 1.0; 
            }

          // DYNAMIC P2P UPDATE: Perfectly executes runtime changes on PointToPoint devices
          DataRate newRate(bwMbps * 1000000);
          txDeviceToHub[src]->SetAttribute ("DataRate", DataRateValue (newRate));
          hubRxDevice[src]->SetAttribute ("DataRate", DataRateValue (newRate));
          
          channelToHub[src]->SetAttribute ("Delay", TimeValue (MilliSeconds (0)));

          Ptr<RateErrorModel> em = CreateObject<RateErrorModel> ();
          em->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          em->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          
          txDeviceToHub[src]->SetAttribute ("ReceiveErrorModel", PointerValue (em));
          hubRxDevice[src]->SetAttribute ("ReceiveErrorModel", PointerValue (em));
        }
    }

  NS_LOG_UNCOND ("Loaded NetworkX Trace Metrics for Sim Time: " << Simulator::Now ().GetSeconds () << "s");

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

// --- 5. Main Network Setup ---
int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  NodeContainer peripheralNodes;
  peripheralNodes.Create (numNodes);

  Ptr<Node> hubNode = CreateObject<Node> ();

  // Build Star Spokes with P2P
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      PointToPointHelper p2p;
      p2p.SetDeviceAttribute ("DataRate", StringValue ("100Mbps")); 
      p2p.SetChannelAttribute ("Delay", StringValue ("0ms"));
      p2p.SetQueue ("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue (QueueSize ("50p")));

      NodeContainer linkNodes (peripheralNodes.Get (i), hubNode);
      NetDeviceContainer devs = p2p.Install (linkNodes);

      Ptr<PointToPointNetDevice> devSpoke = DynamicCast<PointToPointNetDevice> (devs.Get (0));
      Ptr<PointToPointNetDevice> devHubPort = DynamicCast<PointToPointNetDevice> (devs.Get (1));
      Ptr<PointToPointChannel> ch = DynamicCast<PointToPointChannel> (devSpoke->GetChannel ());

      channelToHub[i] = ch;
      txDeviceToHub[i] = devSpoke;
      hubRxDevice[i] = devHubPort; 

      devSpoke->SetPromiscReceiveCallback (MakeCallback (&Ingress_From_Hub));
      devHubPort->SetPromiscReceiveCallback (MakeCallback (&Ingress_At_Hub));
    }

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
  NS_LOG_UNCOND ("ns-3 Dynamic PointToPoint Star Emulation Active (Layer 2 Mode)");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}