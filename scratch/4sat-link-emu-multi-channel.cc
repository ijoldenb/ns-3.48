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

constexpr int numNodes = 4;

// Global tracking matrices for link-by-link customization
Ptr<CsmaChannel> channelMatrix[numNodes][numNodes];
Ptr<CsmaNetDevice> txDeviceMatrix[numNodes][numNodes];
Ptr<CsmaNetDevice> rxDeviceMatrix[numNodes][numNodes];
Ptr<NetDevice> g_fdDev[numNodes];

// Track MAC addresses to cleanly map destination Pis to their internal ns-3 Node IDs
std::map<Mac48Address, uint32_t> macToNodeMap;

// --- Promiscuous Pipelines with Intelligent Destination Switching ---

// 1. Physical VLAN Input -> Look up target node via MAC destination -> Forward down specific link
bool Forward_Fd_to_Space (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  uint32_t srcNodeID = rxDevice->GetNode()->GetId();
  Mac48Address dstMac = Mac48Address::ConvertFrom(dst);

  // If it's a broadcast (ARP) OR we haven't learned where this unicast MAC lives yet, flood it safely
  if (dstMac.IsBroadcast() || macToNodeMap.find(dstMac) == macToNodeMap.end())
    {
      for (int targetNode = 0; targetNode < numNodes; ++targetNode)
        {
          if (targetNode != (int)srcNodeID && txDeviceMatrix[srcNodeID][targetNode] != nullptr)
            {
              Ptr<Packet> pktCopy = packet->Copy();
              txDeviceMatrix[srcNodeID][targetNode]->SendFrom(pktCopy, src, dst, protocol);
            }
        }
      return true;
    }

  // If it's a known unicast destination, send it down the exact variable-latency path
  uint32_t dstNodeID = macToNodeMap[dstMac];
  if (srcNodeID != dstNodeID && txDeviceMatrix[srcNodeID][dstNodeID] != nullptr)
    {
      Ptr<Packet> pktCopy = packet->Copy();
      return txDeviceMatrix[srcNodeID][dstNodeID]->SendFrom(pktCopy, src, dst, protocol);
    }

  return false;
}

// 2. Space Link Input -> Extract frame directly out to the local Pi portal
bool Forward_Space_to_Fd (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  uint32_t nodeID = rxDevice->GetNode()->GetId();
  Ptr<Packet> pktCopy = packet->Copy();
  
  // Learn the source MAC mapping dynamically so unicast routing knows where this Pi is
  Mac48Address srcMac = Mac48Address::ConvertFrom(src);
  if (!srcMac.IsBroadcast())
    {
      macToNodeMap[srcMac] = nodeID;
    }

  return g_fdDev[nodeID]->Send(pktCopy, dst, protocol);
}

// --- Dynamic NetworkX Trace Parser ---
void ParseNextNetworkXSnapshot (std::shared_ptr<std::ifstream> fileStream)
{
  if (fileStream->eof())
    {
      NS_LOG_UNCOND ("End of topology trace file reached.");
      return;
    }

  std::string line, token;
  double nextTimestamp = 0.0;

  // 1. Locate the next TIMESTAMP marker
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

  // 2. Read snapshot metrics line by line
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

      // Calculate dynamic propagation latency based on your trace requirements
      // Note: If you add an explicit delay column to your txt file, extract it here instead!
      double variableDelayMs = 25.0 + (rand() % 50); // Dummy variable calculation placeholder (25-75ms)

      if (src < numNodes && dst < numNodes && channelMatrix[src][dst] != nullptr)
        {
          // Update unique link bandwidth dynamically
          channelMatrix[src][dst]->SetAttribute ("DataRate", DataRateValue (DataRate (bwMbps * 1000000)));
          
          // Update unique link variable latency dynamically!
          channelMatrix[src][dst]->SetAttribute ("Delay", TimeValue (MilliSeconds (variableDelayMs)));

          // Apply unique loss model parameters symmetrically
          Ptr<RateErrorModel> emSD = CreateObject<RateErrorModel> ();
          emSD->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          emSD->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          rxDeviceMatrix[src][dst]->SetAttribute ("ReceiveErrorModel", PointerValue (emSD));

          Ptr<RateErrorModel> emDS = CreateObject<RateErrorModel> ();
          emDS->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          emDS->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          rxDeviceMatrix[dst][src]->SetAttribute ("ReceiveErrorModel", PointerValue (emDS));
        }
    }

  NS_LOG_UNCOND ("Loaded unique metrics for Sim Time: " << Simulator::Now ().GetSeconds () << "s");

  // 3. Schedule next parser cycle execution
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

int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  NodeContainer nodes;
  nodes.Create (numNodes);

  // Initialize tracking matrices
  for (int i = 0; i < numNodes; ++i) {
      for (int j = 0; j < numNodes; ++j) {
          channelMatrix[i][j] = nullptr;
          txDeviceMatrix[i][j] = nullptr;
          rxDeviceMatrix[i][j] = nullptr;
      }
  }

  // 1. Construct matrix mesh skeleton (Independent channels per pair)
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      for (uint32_t j = i + 1; j < numNodes; ++j)
        {
          CsmaHelper csma;
          csma.SetChannelAttribute ("DataRate", StringValue ("10Mbps"));
          csma.SetChannelAttribute ("Delay", StringValue ("0ns"));

          NodeContainer linkNodes (nodes.Get (i), nodes.Get (j));
          NetDeviceContainer devs = csma.Install (linkNodes);

          Ptr<CsmaNetDevice> devI = DynamicCast<CsmaNetDevice> (devs.Get (0));
          Ptr<CsmaNetDevice> devJ = DynamicCast<CsmaNetDevice> (devs.Get (1));
          Ptr<CsmaChannel> ch = DynamicCast<CsmaChannel> (devI->GetChannel ());

          channelMatrix[i][j] = ch;
          channelMatrix[j][i] = ch;
          
          txDeviceMatrix[i][j] = devI;
          txDeviceMatrix[j][i] = devJ;
          
          rxDeviceMatrix[i][j] = devJ; 
          rxDeviceMatrix[j][i] = devI; 

          // Intercept transmissions from space links to catch outbound frames 
          devI->SetPromiscReceiveCallback (MakeCallback (&Forward_Space_to_Fd));
          devJ->SetPromiscReceiveCallback (MakeCallback (&Forward_Space_to_Fd));
        } 
    }

  // 2. Initialize the Emulated Raw File Descriptor Portals
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));

  std::vector<std::string> vlanMapping = {"vlan101", "vlan102", "vlan103", "vlan104"};

  for (int i = 0; i < numNodes; ++i)
    {
      emuHelper.SetDeviceName (vlanMapping[i]);
      
      NetDeviceContainer devSide = emuHelper.Install (nodes.Get(i));
      g_fdDev[i] = devSide.Get(0);
      g_fdDev[i]->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

      // Pipeline incoming data from physical world to space mapping engine
      g_fdDev[i]->SetPromiscReceiveCallback (MakeCallback (&Forward_Fd_to_Space));
    }

  // 3. Load trace stream file loop
  auto traceFile = std::make_shared<std::ifstream> ("/home/ijoldenb/ns-3.48/scratch/topology_trace.txt");
  if (!traceFile->is_open ())
    {
      NS_FATAL_ERROR ("Could not open topology_trace.txt!");
    }
  
  Simulator::Schedule (Seconds (0.0), &ParseNextNetworkXSnapshot, traceFile);

  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 4-Node Dynamic Matrix-Latency Emulation Active");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}