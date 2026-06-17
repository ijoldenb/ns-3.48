#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/bridge-module.h"
#include "ns3/fd-net-device-module.h"

using namespace ns3;

void KeepAliveDummyEvent () {}

int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  // 1. Enforce real-time clock synchronization for Hardware-in-the-Loop execution
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  // 2. Create ONE single router/satellite proxy node
  NodeContainer nodes;
  nodes.Create (1);
  Ptr<Node> proxyNode = nodes.Get(0);

  // 3. Build the Raw Emulated NetDevices to latch onto your VM sub-interfaces
  EmuFdNetDeviceHelper emuHelper;

  // Real-world Ingress: VLAN 101 (Pi 1 Side)
  emuHelper.SetDeviceName ("enp0s8.101");
  NetDeviceContainer devSideA = emuHelper.Install (proxyNode);
  Ptr<NetDevice> netDevA = devSideA.Get(0);

  // Real-world Egress: VLAN 102 (Pi 2 Side)
  emuHelper.SetDeviceName ("enp0s8.102");
  NetDeviceContainer devSideB = emuHelper.Install (proxyNode);
  Ptr<NetDevice> netDevB = devSideB.Get(0);

  // 4. Create the Internal Bridge Device to stitch the two worlds together
  BridgeHelper bridgeHelper;
  NetDeviceContainer bridgeDevices;
  bridgeDevices.Add (netDevA);
  bridgeDevices.Add (netDevB);
  
  // Install the software bridge onto our single node, making it a live wire
  NetDeviceContainer mainBridge = bridgeHelper.Install (proxyNode, bridgeDevices);

  // 5. Configure your Satellite Link metrics directly on the raw forwarding pipes
  // We can apply channel error, delay, or data rate directly to the FdNetDevices if needed.
  // For standard transparent L2 pass-through, the bridge handles immediate wire-speed delivery.

  // 6. Execution Windows and Keep-Alives
  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 Layer 2 Transparent Bridge Engine is LIVE!");
  NS_LOG_UNCOND ("Bridging enp0s8.101 <---> enp0s8.102 across the simulation loop.");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}