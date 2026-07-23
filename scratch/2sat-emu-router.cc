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

  // 1. Force real-time hardware clock synchronization 
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  // 2. Create one single router/satellite proxy node
  NodeContainer nodes;
  nodes.Create (1);
  Ptr<Node> proxyNode = nodes.Get(0);

  // 3. Initialize the Emulated Raw File Descriptor Helper
  EmuFdNetDeviceHelper emuHelper;

  // Latch Node Port A straight onto physical VLAN 101
  emuHelper.SetDeviceName ("vlan101");
  NetDeviceContainer devSideA = emuHelper.Install (proxyNode);
  Ptr<NetDevice> netDevA = devSideA.Get(0);
  netDevA->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

  // Latch Node Port B straight onto physical VLAN 102
  emuHelper.SetDeviceName ("vlan102");
  NetDeviceContainer devSideB = emuHelper.Install (proxyNode);
  Ptr<NetDevice> netDevB = devSideB.Get(0);
  netDevB->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

  // 4. Create an Internal Bridge to pass packets between the raw sockets
  BridgeHelper bridgeHelper;
  NetDeviceContainer bridgeDevices;
  bridgeDevices.Add (netDevA);
  bridgeDevices.Add (netDevB);
  
  // Tie them together. This node now acts like an intelligent physical switch layer
  NetDeviceContainer mainBridge = bridgeHelper.Install (proxyNode, bridgeDevices);

  // 5. Run parameters (1 Hour 40 Mins window)
  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 Raw FdNetDevice Transparent Bridge Engine");
  NS_LOG_UNCOND ("Directly sniffing vlan101 <---> vlan102");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}