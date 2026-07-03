#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/error-model.h"
#include "ns3/bridge-module.h"
#include <vector>
#include <string>
#include <fstream>
#include <sstream>

using namespace ns3;

constexpr int numNodes = 4; // Total number of nodes in the simulation

// Global tracking matrices
Ptr<CsmaChannel> channelMatrix[numNodes][numNodes];
Ptr<CsmaNetDevice> rxDeviceMatrix[numNodes][numNodes];

// --- The NetworkX Edge-List Parser Engine ---
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

  // 2. Read lines until we hit the "END" token for this snapshot
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

      // 3. Apply the dynamic NetworkX metrics to the active full mesh
      if (src < numNodes && dst < numNodes && channelMatrix[src][dst] != nullptr)
        {
          // Update channel bandwidth dynamically
          channelMatrix[src][dst]->SetAttribute ("DataRate", DataRateValue (DataRate (bwMbps * 1000000)));

          // Apply loss model to Source -> Destination interface
          Ptr<RateErrorModel> emSD = CreateObject<RateErrorModel> ();
          emSD->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          emSD->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          rxDeviceMatrix[src][dst]->SetAttribute ("ReceiveErrorModel", PointerValue (emSD));

          // Apply loss model to Destination -> Source interface (Symmetric)
          Ptr<RateErrorModel> emDS = CreateObject<RateErrorModel> ();
          emDS->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          emDS->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          rxDeviceMatrix[dst][src]->SetAttribute ("ReceiveErrorModel", PointerValue (emDS));
        }
    }

  NS_LOG_UNCOND ("Loaded unique NetworkX metrics for Sim Time: " << Simulator::Now ().GetSeconds () << "s");

  // 4. Schedule next step execution
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

  std::vector<NetDeviceContainer> nodeBridgePorts (numNodes);

  // Initialize tracking matrices to null
  for (int i = 0; i < numNodes; ++i) {
      for (int j = 0; j < numNodes; ++j) {
          channelMatrix[i][j] = nullptr;
          rxDeviceMatrix[i][j] = nullptr;
      }
  }

  // 1. Construct topology skeleton with zero propagation delay
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      for (uint32_t j = i + 1; j < numNodes; ++j)
        {
          CsmaHelper csma;
          csma.SetChannelAttribute ("DataRate", StringValue ("10Mbps")); // Safe baseline initialization speed
          csma.SetChannelAttribute ("Delay", StringValue ("0ns"));       // Explicitly zero delay

          NodeContainer linkNodes (nodes.Get (i), nodes.Get (j));
          NetDeviceContainer devs = csma.Install (linkNodes);

          Ptr<CsmaNetDevice> devI = DynamicCast<CsmaNetDevice> (devs.Get (0));
          Ptr<CsmaNetDevice> devJ = DynamicCast<CsmaNetDevice> (devs.Get (1));
          Ptr<CsmaChannel> ch = DynamicCast<CsmaChannel> (devI->GetChannel ());

          channelMatrix[i][j] = ch;
          channelMatrix[j][i] = ch;
          rxDeviceMatrix[i][j] = devJ; 
          rxDeviceMatrix[j][i] = devI; 

          nodeBridgePorts[i].Add (devI);
          nodeBridgePorts[j].Add (devJ);
        } 
    }

  // 2. Map VLAN Portals
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));

  for (uint32_t i = 0; i < numNodes; ++i)
    {
      std::string interfaceName = "vlan" + std::to_string (101 + i);
      emuHelper.SetDeviceName (interfaceName);

      NetDeviceContainer fdDevs = emuHelper.Install (nodes.Get (i));
      Ptr<NetDevice> fdDev = fdDevs.Get (0);
      fdDev->SetAttribute ("Address", Mac48AddressValue (Mac48Address::Allocate ()));

      nodeBridgePorts[i].Add (fdDev);
    }

  // 3. Install Layer-2 Learning Bridges (Learning ENABLED)
  // Since all paths remain physically alive, learning handles routing seamlessly without looping
  BridgeHelper bridge;
  for (uint32_t i = 0; i < numNodes; ++i)
    {
      bridge.Install (nodes.Get (i), nodeBridgePorts[i]);
    }

  // 4. Open trace file and launch scheduling loop
  auto traceFile = std::make_shared<std::ifstream> ("/home/ijoldenb/ns-3.48/scratch/topology_trace.txt");
  if (!traceFile->is_open ())
    {
      NS_FATAL_ERROR ("Could not open topology_trace.txt!");
    }
  
  Simulator::Schedule (Seconds (0.0), &ParseNextNetworkXSnapshot, traceFile);

  Time stopTime = Seconds (3600.0); 
  Simulator::Stop (stopTime);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 " + std::to_string (numNodes) + "-Node Full-Mesh Satellite Emulation Active");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}