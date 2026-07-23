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

// --- Full Mesh Tracking Matrices ---
// meshDevices[SrcNode][DstNode] gives the exact PointToPoint device transmitting FROM Src TO Dst.
Ptr<PointToPointNetDevice> meshDevices[numNodes][numNodes];   
Ptr<NetDevice> g_fdDev[numNodes];

// Global MAC Table for distributed switching
std::map<Mac48Address, uint32_t> macToNodeMap;

// --- 1. Custom Encapsulation Tunnel ---
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

// --- 2. Distributed Switch Ingress (From Physical Raspberry Pi) ---
bool Ingress_From_Vlan (uint32_t myNodeID, Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                         const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Mac48Address srcMac = Mac48Address::ConvertFrom(src);
  Mac48Address dstMac = Mac48Address::ConvertFrom(dst);

  // Learn where this Pi's MAC address lives
  if (!srcMac.IsBroadcast())
    {
      macToNodeMap[srcMac] = myNodeID;
    }

  // If Broadcast/Multicast (like ARP or OSPF), flood to all OTHER nodes in the mesh
  if (dstMac.IsBroadcast() || dstMac.IsGroup())
    {
      for (uint32_t i = 0; i < numNodes; ++i)
        {
          if (i != myNodeID && meshDevices[myNodeID][i] != nullptr)
            {
              SendOverP2PTunnel (meshDevices[myNodeID][i], packet, protocol, src, dst);
            }
        }
    }
  else // Unicast
    {
      if (macToNodeMap.find(dstMac) != macToNodeMap.end())
        {
          uint32_t targetNode = macToNodeMap[dstMac];
          // Forward directly down the dedicated mesh wire to the destination
          if (targetNode != myNodeID && meshDevices[myNodeID][targetNode] != nullptr)
            {
              SendOverP2PTunnel (meshDevices[myNodeID][targetNode], packet, protocol, src, dst);
            }
        }
      else
        {
          // Unknown unicast: Flood to all other nodes until MAC is learned
          for (uint32_t i = 0; i < numNodes; ++i)
            {
              if (i != myNodeID && meshDevices[myNodeID][i] != nullptr)
                {
                  SendOverP2PTunnel (meshDevices[myNodeID][i], packet, protocol, src, dst);
                }
            }
        }
    }
  return true;
}

// --- 3. Split-Horizon Ingress (From ns-3 Mesh) ---
bool Ingress_From_Mesh (uint32_t myNodeID, Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                        const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy();
  EthernetHeader eth;
  pktCopy->RemoveHeader (eth);
  
  // SPLIT HORIZON RULE: Packets arriving from the mesh are NEVER forwarded back into the mesh.
  // They are only sent UP to the physical Raspberry Pi attached to this node. 
  // This completely eliminates Layer 2 broadcast loops without needing STP.
  g_fdDev[myNodeID]->Send(pktCopy, eth.GetDestination(), eth.GetLengthType());
  
  return true;
}

// --- 4. Scheduled Apply Function (Strict Directional Mapping) ---
struct LinkChange {
  uint32_t src;
  uint32_t dst;
  double bwMbps;
  double dropRate;
};

void ApplyLinkChanges (std::vector<LinkChange> changes)
{
  NS_LOG_UNCOND ("\n--- Sim Time: " << Simulator::Now ().GetSeconds () << "s | Applying YAML Topology Updates ---");
  for (const auto& change : changes)
    {
      if (change.src < numNodes && change.dst < numNodes && change.src != change.dst)
        {
          double bw = change.bwMbps <= 0.0 ? 0.000001 : change.bwMbps;
          double drop = change.bwMbps <= 0.0 ? 1.0 : change.dropRate;

          DataRate newRate(bw * 1000000);
          
          // Create error model for the Destination node receiving from Source
          Ptr<RateErrorModel> emDst = CreateObject<RateErrorModel> ();
          emDst->SetAttribute ("ErrorRate", DoubleValue (drop));
          emDst->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);

          // Create error model for the Source node receiving from Destination
          Ptr<RateErrorModel> emSrc = CreateObject<RateErrorModel> ();
          emSrc->SetAttribute ("ErrorRate", DoubleValue (drop));
          emSrc->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);

          // --- FIX: Apply updates symmetrically to both sides of the P2P pipe ---
          
          // 1. Throttle the Forward Path (Src -> Dst)
          meshDevices[change.src][change.dst]->SetAttribute ("DataRate", DataRateValue (newRate));
          meshDevices[change.dst][change.src]->SetAttribute ("ReceiveErrorModel", PointerValue (emDst));
          
          // 2. Throttle the Return Path (Dst -> Src)
          meshDevices[change.dst][change.src]->SetAttribute ("DataRate", DataRateValue (newRate));
          meshDevices[change.src][change.dst]->SetAttribute ("ReceiveErrorModel", PointerValue (emSrc));

          NS_LOG_UNCOND ("  [Link Updated Symmetrically] " << (change.src + 1) << " <-> " << (change.dst + 1) 
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

  NodeContainer meshNodes;
  meshNodes.Create (numNodes);

  // Initialize tracking matrix
  for(int i=0; i<numNodes; i++)
    for(int j=0; j<numNodes; j++)
      meshDevices[i][j] = nullptr;

  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("100Mbps")); 
  p2p.SetChannelAttribute ("Delay", StringValue ("0ms"));
  p2p.SetQueue ("ns3::DropTailQueue<Packet>", "MaxSize", QueueSizeValue (QueueSize ("5000p")));

  // Build the Full Mesh (Creates N*(N-1)/2 Links)
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      for (uint32_t j = i + 1; j < numNodes; ++j)
        {
          NodeContainer linkNodes (meshNodes.Get (i), meshNodes.Get (j));
          NetDeviceContainer devs = p2p.Install (linkNodes);

          Ptr<PointToPointNetDevice> devI = DynamicCast<PointToPointNetDevice> (devs.Get (0));
          Ptr<PointToPointNetDevice> devJ = DynamicCast<PointToPointNetDevice> (devs.Get (1));

          meshDevices[i][j] = devI; // Device on 'i' transmitting to 'j'
          meshDevices[j][i] = devJ; // Device on 'j' transmitting to 'i'

          // Bind the receiving callbacks (passing the local Node ID)
          devI->SetPromiscReceiveCallback (MakeBoundCallback (&Ingress_From_Mesh, i));
          devJ->SetPromiscReceiveCallback (MakeBoundCallback (&Ingress_From_Mesh, j));
        }
    }

  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));
  std::vector<std::string> vlanMapping = {"vlan101", "vlan102", "vlan103", "vlan104"};

  for (int i = 0; i < numNodes; ++i)
    {
      emuHelper.SetDeviceName (vlanMapping[i]);
      
      NetDeviceContainer devSide = emuHelper.Install (meshNodes.Get(i));
      g_fdDev[i] = devSide.Get(0);
      g_fdDev[i]->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

      // Bind the Pi ingest callback
      g_fdDev[i]->SetPromiscReceiveCallback (MakeBoundCallback (&Ingress_From_Vlan, i));
    }

  // --- Parse YAML File ---
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
      if (line.empty() || line.find (":") == std::string::npos) continue;

      try 
        {
          std::string valStr = line.substr (line.find (":") + 1);

          if (line.find ("- time:") != std::string::npos)
            {
              scheduleTime = std::stod (valStr);
            }
          else if (line.find ("- src:") != std::string::npos)
            {
              // Subtract 1 to map YAML Node 1-4 to internal Node 0-3
              tempChange.src = std::stoi (valStr) - 1; 
              inLink = true;
            }
          else if (inLink && line.find ("dst:") != std::string::npos)
            {
              // Subtract 1 to map YAML Node 1-4 to internal Node 0-3
              tempChange.dst = std::stoi (valStr) - 1; 
            }
          else if (inLink && line.find ("bw:") != std::string::npos)
            {
              tempChange.bwMbps = std::stod (valStr);
            }
          else if (inLink && line.find ("drop:") != std::string::npos)
            {
              tempChange.dropRate = std::stod (valStr);
              
              timelineMap[scheduleTime].push_back (tempChange);
              inLink = false;
              tempChange = LinkChange(); // Reset to prevent bleeding
            }
        } 
      catch (const std::exception& e) 
        {
          NS_FATAL_ERROR ("\n[YAML PARSE ERROR] Crash on line: '" << line << "'\nException: " << e.what ());
        }
    }

  for (auto const& entry : timelineMap)
    {
      Simulator::Schedule (Seconds (entry.first), &ApplyLinkChanges, entry.second);
    }

  Time stopTime = Seconds (3600.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 Dynamic PointToPoint FULL MESH Active (L2 Split-Horizon)");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}