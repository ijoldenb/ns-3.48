#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/error-model.h"
#include "ns3/ethernet-header.h"
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
void SendOverP2PTunnel (Ptr<NetDevice> dev, Ptr<const Packet> packet, uint16_t protocol, const Address &src, const Address &dst)
{
  Ptr<Packet> pktCopy = packet->Copy();
  EthernetHeader eth;
  eth.SetSource (Mac48Address::ConvertFrom (src));
  eth.SetDestination (Mac48Address::ConvertFrom (dst));
  eth.SetLengthType (protocol);
  
  pktCopy->AddHeader (eth);
  dev->Send (pktCopy, dev->GetBroadcast (), 0x0800); 
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

bool Ingress_From_Hub (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                        const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy();
  EthernetHeader eth;
  pktCopy->RemoveHeader (eth);
  
  SwitchPacket (rxDevice, pktCopy, eth.GetLengthType(), eth.GetSource(), eth.GetDestination(), false, -1);
  return true;
}

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

// --- 4. Scheduled Apply Function ---
struct LinkChange {
  uint32_t src;
  uint32_t dst;
  double bwMbps;
  double dropRate;
};

// This function is perfectly mapped to the ns-3 scheduler. It will trigger at the exact time required.
void ApplyLinkChanges (std::vector<LinkChange> changes)
{
  NS_LOG_UNCOND ("\n--- Sim Time: " << Simulator::Now ().GetSeconds () << "s | Applying YAML Topology Updates ---");
  for (const auto& change : changes)
    {
      if (change.src < numNodes && channelToHub[change.src] != nullptr)
        {
          double bw = change.bwMbps <= 0.0 ? 0.000001 : change.bwMbps;
          double drop = change.bwMbps <= 0.0 ? 1.0 : change.dropRate;

          DataRate newRate(bw * 1000000);
          txDeviceToHub[change.src]->SetAttribute ("DataRate", DataRateValue (newRate));
          hubRxDevice[change.src]->SetAttribute ("DataRate", DataRateValue (newRate));

          Ptr<RateErrorModel> em = CreateObject<RateErrorModel> ();
          em->SetAttribute ("ErrorRate", DoubleValue (drop));
          em->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          
          txDeviceToHub[change.src]->SetAttribute ("ReceiveErrorModel", PointerValue (em));
          hubRxDevice[change.src]->SetAttribute ("ReceiveErrorModel", PointerValue (em));

          // ---> NEW: Log the exact changes for this link <---
          NS_LOG_UNCOND ("  [Link Updated] Src Node " << change.src << " -> Dst Node " << change.dst 
                         << " | BW: " << bw << " Mbps | Drop Rate: " << drop);
        }
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

  for (uint32_t i = 0; i < numNodes; ++i)
    {
      PointToPointHelper p2p;
      p2p.SetDeviceAttribute ("DataRate", StringValue ("100Mbps")); 
      p2p.SetChannelAttribute ("Delay", StringValue ("0ms"));
      p2p.SetQueue ("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue (QueueSize ("5000p")));

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

  // Front-Load YAML Parsing & Event Scheduling
  std::ifstream traceFile ("/home/ijoldenb/ns-3.48/scratch/topology_trace.yaml");
  if (!traceFile.is_open ())
    {
      NS_FATAL_ERROR ("Could not open topology_trace.yaml!");
    }
  
  std::string line;
  double scheduleTime = 0.0;
  bool inLink = false;
  LinkChange tempChange;
  std::map<double, std::vector<LinkChange>> timelineMap;

  while (std::getline (traceFile, line))
      {
        // Skip empty lines or lines without a colon (like the "links:" header)
        if (line.empty() || line.find (":") == std::string::npos) continue;

        try 
          {
            // Extract everything after the colon
            std::string valStr = line.substr (line.find (":") + 1);

            if (line.find ("- time:") != std::string::npos)
              {
                scheduleTime = std::stod (valStr);
              }
            else if (line.find ("- src:") != std::string::npos)
              {
                tempChange.src = std::stoi (valStr);
                inLink = true;
              }
            else if (inLink && line.find ("dst:") != std::string::npos)
              {
                tempChange.dst = std::stoi (valStr);
              }
            else if (inLink && line.find ("bw:") != std::string::npos)
              {
                tempChange.bwMbps = std::stod (valStr);
              }
            else if (inLink && line.find ("drop:") != std::string::npos)
              {
                tempChange.dropRate = std::stod (valStr);
                
                // The block is finished; save it to the current time map
                timelineMap[scheduleTime].push_back (tempChange);
                inLink = false;
              }
          } 
        catch (const std::exception& e) 
          {
            // If stod or stoi crashes, print exactly what string it choked on
            NS_FATAL_ERROR ("\n\n[YAML PARSE ERROR] Crash on line: '" << line << "'\n"
                            << "Attempted to parse this value as a number: '" << line.substr (line.find (":") + 1) << "'\n"
                            << "Exception: " << e.what () << "\n"
                            << "Please check topology_trace.yaml to ensure this value is a valid number.\n");
          }
      }

  // Bind the mapped data to the simulator perfectly on time
  for (auto const& entry : timelineMap)
    {
      Simulator::Schedule (Seconds (entry.first), &ApplyLinkChanges, entry.second);
    }

  Time stopTime = Seconds (3600.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 Dynamic PointToPoint Star Emulation Active (YAML Mode)");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}