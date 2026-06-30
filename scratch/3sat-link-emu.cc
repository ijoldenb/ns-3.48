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

// 20x20 Global tracking matrices for the unique links
Ptr<CsmaChannel> channelMatrix[20][20];
Ptr<CsmaNetDevice> rxDeviceMatrix[20][20];

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

      // 3. Apply the unique NetworkX properties to this specific link pair
      if (src < 20 && dst < 20 && channelMatrix[src][dst] != nullptr)
        {
          // Update unique channel bandwidth
          channelMatrix[src][dst]->SetAttribute ("DataRate", DataRateValue (DataRate (bwMbps * 1000000)));

          // Apply unique loss model to Source -> Destination interface
          Ptr<RateErrorModel> emSD = CreateObject<RateErrorModel> ();
          emSD->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          emSD->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          rxDeviceMatrix[src][dst]->SetAttribute ("ReceiveErrorModel", PointerValue (emSD));

          // Apply unique loss model to Destination -> Source interface (Symmetric)
          Ptr<RateErrorModel> emDS = CreateObject<RateErrorModel> ();
          emDS->SetAttribute ("ErrorRate", DoubleValue (dropRate));
          emDS->SetUnit (RateErrorModel::ERROR_UNIT_PACKET);
          rxDeviceMatrix[dst][src]->SetAttribute ("ReceiveErrorModel", PointerValue (emDS));
        }
    }

  NS_LOG_UNCOND ("Loaded unique NetworkX metrics for Sim Time: " << Simulator::Now ().GetSeconds () << "s");

  // 4. Schedule the next file-read step execution dynamically
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

// --- Main Simulation ---
int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  NodeContainer nodes;
  nodes.Create (20);

  std::vector<NetDeviceContainer> nodeBridgePorts (20);

  // Initialize tracking matrices to null
  for (int i = 0; i < 20; ++i) {
      for (int j = 0; j < 20; ++j) {
          channelMatrix[i][j] = nullptr;
          rxDeviceMatrix[i][j] = nullptr;
      }
  }

  // 1. Construct the internal point-to-point isolated mesh topology
  for (uint32_t i = 0; i < 20; ++i)
    {
      for (uint32_t j = i + 1; j < 20; ++j)
        {
          // Creating a brand new helper instance inside the loop forces ns-3 
          // to allocate a completely independent channel object for this pair.
          CsmaHelper csma;
          csma.SetChannelAttribute ("DataRate", StringValue ("1Mbps")); // low dummy baseline
          csma.SetChannelAttribute ("Delay", StringValue ("1ms"));

          NodeContainer linkNodes (nodes.Get (i), nodes.Get (j));
          NetDeviceContainer devs = csma.Install (linkNodes);

          Ptr<CsmaNetDevice> devI = DynamicCast<CsmaNetDevice> (devs.Get (0));
          Ptr<CsmaNetDevice> devJ = DynamicCast<CsmaNetDevice> (devs.Get (1));
          Ptr<CsmaChannel> ch = DynamicCast<CsmaChannel> (devI->GetChannel ());

          // Map the unique channel and device pointers to our matrix
          channelMatrix[i][j] = ch;
          channelMatrix[j][i] = ch;
          rxDeviceMatrix[i][j] = devJ; // Device on J receiving from I
          rxDeviceMatrix[j][i] = devI; // Device on I receiving from J

          nodeBridgePorts[i].Add (devI);
          nodeBridgePorts[j].Add (devJ);
        }
    }

  // 2. Map Hardware-in-the-Loop VLAN Portals
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));

  for (uint32_t i = 0; i < 20; ++i)
    {
      std::string interfaceName = "vlan" + std::to_string (101 + i);
      emuHelper.SetDeviceName (interfaceName);

      NetDeviceContainer fdDevs = emuHelper.Install (nodes.Get (i));
      Ptr<NetDevice> fdDev = fdDevs.Get (0);
      fdDev->SetAttribute ("Address", Mac48AddressValue (Mac48Address::Allocate ()));

      nodeBridgePorts[i].Add (fdDev);
    }

  // 3. Install pure Layer-2 Learning Bridges
  BridgeHelper bridge;
  for (uint32_t i = 0; i < 20; ++i)
    {
      bridge.Install (nodes.Get (i), nodeBridgePorts[i]);
    }

  // 4. Open your NetworkX trace file and kick off the scheduling engine
  auto traceFile = std::make_shared<std::ifstream> ("topology_trace.txt");
  if (!traceFile->is_open ())
    {
      NS_FATAL_ERROR ("Could not open topology_trace.txt! Ensure it matches your Python output directory.");
    }
  
  // Schedule the very first file read at 0.0 seconds
  Simulator::Schedule (Seconds (0.0), &ParseNextNetworkXSnapshot, traceFile);

  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 20-Node Independent Link HIL Satellite Emulation Engine");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}