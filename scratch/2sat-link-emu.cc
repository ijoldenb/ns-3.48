#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/csma-module.h"
#include "ns3/error-model.h"

using namespace ns3;

void KeepAliveDummyEvent () {}

// File-scope global device pointers for safe callback execution
Ptr<NetDevice> g_fdDevA;
Ptr<NetDevice> g_p2pDevA;
Ptr<NetDevice> g_fdDevB;
Ptr<NetDevice> g_p2pDevB;

// 1. Node A: Physical vlan101 Input -> Forward to Space Uplink (Preserves MACs)
bool Forward_FdA_to_P2PA (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy ();
  return g_p2pDevA->SendFrom (pktCopy, src, dst, protocol);
}

// 2. Node A: Space Downlink -> Forward to Physical vlan101 Output (Raw Socket Write)
bool Forward_P2PA_to_FdA (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy ();
  return g_fdDevA->Send (pktCopy, dst, protocol);
}

// 3. Node B: Physical vlan102 Input -> Forward to Space Uplink (Preserves MACs)
bool Forward_FdB_to_P2PB (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy ();
  return g_p2pDevB->SendFrom (pktCopy, src, dst, protocol);
}

// 4. Node B: Space Downlink -> Forward to Physical vlan102 Output (Raw Socket Write)
bool Forward_P2PB_to_FdB (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  Ptr<Packet> pktCopy = packet->Copy ();
  return g_fdDevB->Send (pktCopy, dst, protocol);
}

int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  // 1. Force real-time hardware clock synchronization 
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  // 2. Create TWO separate internal nodes representing your space transponders
  NodeContainer nodes;
  nodes.Create (2);
  Ptr<Node> nodeA = nodes.Get(0);
  Ptr<Node> nodeB = nodes.Get(1);

  CsmaHelper csma;
  csma.SetChannelAttribute ("DataRate", StringValue ("100Mbps"));
  //csma.SetChannelAttribute ("Delay", StringValue ("1ms"));
  NetDeviceContainer p2pDevices = csma.Install (nodes);

  Ptr<RateErrorModel> errorModel = CreateObject<RateErrorModel> ();
  errorModel->SetAttribute ("ErrorRate", DoubleValue (0.1));
  errorModel->SetAttribute ("ErrorUnit", StringValue ("ERROR_UNIT_PACKET"));
  p2pDevices.Get(0)->SetAttribute ("ReceiveErrorModel", PointerValue (errorModel)); // Only applys to nodeB's P2P device to simulate downlink errors
  p2pDevices.Get(1)->SetAttribute ("ReceiveErrorModel", PointerValue (errorModel));

  g_p2pDevA = p2pDevices.Get (0);
  g_p2pDevB = p2pDevices.Get (1);

  // 4. Initialize the Emulated Raw File Descriptor Helper
  EmuFdNetDeviceHelper emuHelper;
  
  // Force DIX (Ethernet II) framing to preserve raw frame integrity over the OS wire
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));

  // Latch Node A Port straight onto physical VLAN 101 (Pi 1 Side)
  emuHelper.SetDeviceName ("vlan101");
  NetDeviceContainer devSideA = emuHelper.Install (nodeA);
  g_fdDevA = devSideA.Get(0);
  g_fdDevA->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

  // Latch Node B Port straight onto physical VLAN 102 (Pi 2 Side)
  emuHelper.SetDeviceName ("vlan102");
  NetDeviceContainer devSideB = emuHelper.Install (nodeB);
  g_fdDevB = devSideB.Get(0);
  g_fdDevB->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

  // 5. Connect the Pipelines via native Promiscuous Callbacks
  // Node A Pipeline Hooks
  g_fdDevA->SetPromiscReceiveCallback (MakeCallback (&Forward_FdA_to_P2PA));
  g_p2pDevA->SetPromiscReceiveCallback (MakeCallback (&Forward_P2PA_to_FdA));

  // Node B Pipeline Hooks
  g_fdDevB->SetPromiscReceiveCallback (MakeCallback (&Forward_FdB_to_P2PB));
  g_p2pDevB->SetPromiscReceiveCallback (MakeCallback (&Forward_P2PB_to_FdB));

  csma.EnablePcapAll("satellite-emulation", true);
  emuHelper.EnablePcapAll("satellite-link-emulation", true);

  // 7. Run parameters (1 Hour 40 Mins window)
  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 Hardware-In-The-Loop Satellite Simulator");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}