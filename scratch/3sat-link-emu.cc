#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/csma-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/error-model.h"
#include "ns3/bridge-module.h" // Required for native L2 bridging
#include <vector>
#include <string>

using namespace ns3;

// Global tracking matrices for your NetworkX matrix parser to access later
Ptr<CsmaChannel> channelMatrix[20][20];
Ptr<CsmaNetDevice> rxDeviceMatrix[20][20];

int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  // 1. Force real-time hardware clock synchronization for your physical Pis
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  // 2. Create the 20 internal nodes
  NodeContainer nodes;
  nodes.Create (20);

  // Maintain a collection of net devices per node so we can bundle them into bridges later
  std::vector<NetDeviceContainer> nodeBridgePorts(20);

  // 3. Construct the internal 190-link point-to-point mesh topology
  for (uint32_t i = 0; i < 20; ++i)
    {
      for (uint32_t j = i + 1; j < 20; ++j)
        {
          CsmaHelper csma;
          csma.SetChannelAttribute ("DataRate", StringValue ("100Mbps"));
          csma.SetChannelAttribute ("Delay", StringValue ("1ms"));

          NodeContainer linkNodes (nodes.Get (i), nodes.Get (j));
          NetDeviceContainer devs = csma.Install (linkNodes);

          Ptr<CsmaNetDevice> devI = DynamicCast<CsmaNetDevice> (devs.Get (0));
          Ptr<CsmaNetDevice> devJ = DynamicCast<CsmaNetDevice> (devs.Get (1));
          Ptr<CsmaChannel> ch = DynamicCast<CsmaChannel> (devI->GetChannel());

          // Save references to the global matrix so your file-reader can change link metrics dynamically
          channelMatrix[i][j] = ch;
          channelMatrix[j][i] = ch;
          rxDeviceMatrix[i][j] = devJ;
          rxDeviceMatrix[j][i] = devI;

          // Queue these devices to be attached to each node's local learning bridge
          nodeBridgePorts[i].Add (devI);
          nodeBridgePorts[j].Add (devJ);
        }
    }

  // 4. Initialize and map your Hardware-in-the-Loop VLAN Portals
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix")); // Enforce Ethernet II

  for (uint32_t i = 0; i < 20; ++i)
    {
      // Generates target interface handles dynamically: vlan101, vlan102, ..., vlan120
      std::string interfaceName = "vlan" + std::to_string (101 + i);
      emuHelper.SetDeviceName (interfaceName);

      NetDeviceContainer fdDevs = emuHelper.Install (nodes.Get (i));
      Ptr<NetDevice> fdDev = fdDevs.Get (0);
      
      // Assign a unique MAC address to the proxy portal interface
      fdDev->SetAttribute ("Address", Mac48AddressValue (Mac48Address::Allocate ()));

      // Add the physical raw socket portal into this node's bridge port list
      nodeBridgePorts[i].Add (fdDev);
    }

  // 5. The Magic Step: Install pure Layer-2 Learning Bridges
  // This step completely replaces your manual Forwarding Functions!
  BridgeHelper bridge;
  for (uint32_t i = 0; i < 20; ++i)
    {
      // This instantiates an internal L2 switch on Node i, bridging its 19 internal
      // mesh connections and its 1 external physical Pi connection seamlessly.
      bridge.Install (nodes.Get (i), nodeBridgePorts[i]);
    }

  // 6. Tracing and Logging Metrics
  emuHelper.EnablePcapAll ("satellite-link-emulation", true);

  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 20-Node Scaled HIL Satellite Emulation Engine (L2 Bridged)");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}