#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/csma-module.h"
#include "ns3/error-model.h"

using namespace ns3;

void KeepAliveDummyEvent () {}

constexpr int numNodes = 4;

// File-scope global device arrays for 1-to-1 mapping
Ptr<NetDevice> g_fdDev[numNodes];
Ptr<NetDevice> g_spaceDev[numNodes];

// 1. Physical VLAN Input -> Forward straight onto the Shared Space Bus
bool Forward_Fd_to_Space (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  uint32_t nodeID = rxDevice->GetNode()->GetId();
  Ptr<Packet> pktCopy = packet->Copy ();
  return g_spaceDev[nodeID]->SendFrom (pktCopy, src, dst, protocol);
}

// 2. Shared Space Bus Input -> Forward straight out to the node's local Physical VLAN
bool Forward_Space_to_Fd (Ptr<NetDevice> rxDevice, Ptr<const Packet> packet, uint16_t protocol,
                          const Address &src, const Address &dst, NetDevice::PacketType packetType)
{
  uint32_t nodeID = rxDevice->GetNode()->GetId();
  Ptr<Packet> pktCopy = packet->Copy ();
  return g_fdDev[nodeID]->Send (pktCopy, dst, protocol);
}

int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  // Force real-time hardware clock synchronization 
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  // Create FOUR separate internal nodes representing your space transponders
  NodeContainer nodes;
  nodes.Create (numNodes);

  // 3. CRITICAL: Install a SINGLE CSMA channel across ALL nodes to create a shared bus
  CsmaHelper csma;
  csma.SetChannelAttribute ("DataRate", StringValue ("100Mbps"));
  csma.SetChannelAttribute ("Delay", StringValue ("0ns"));

  // This single line binds all 4 nodes to the exact same virtual wire backend
  NetDeviceContainer spaceDevices = csma.Install (nodes);

  for (int i = 0; i < numNodes; ++i)
    {
      Ptr<RateErrorModel> errorModel = CreateObject<RateErrorModel> ();
      errorModel->SetAttribute ("ErrorRate", DoubleValue (0.0));
      errorModel->SetAttribute ("ErrorUnit", StringValue ("ERROR_UNIT_PACKET"));
      spaceDevices.Get(i)->SetAttribute ("ReceiveErrorModel", PointerValue (errorModel));

      // Map the space device for this specific node
      g_spaceDev[i] = spaceDevices.Get(i);
    }

  // 4. Initialize the Emulated Raw File Descriptor Helper for 4 VLAN portals
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetAttribute ("EncapsulationMode", StringValue ("Dix"));

  for (int i = 0; i < numNodes; ++i)
    {
      std::string vlanName = "vlan" + std::to_string(101 + i);
      emuHelper.SetDeviceName (vlanName);
      
      NetDeviceContainer devSide = emuHelper.Install (nodes.Get(i));
      g_fdDev[i] = devSide.Get(0);
      g_fdDev[i]->SetAttribute("Address", Mac48AddressValue(Mac48Address::Allocate()));

      // 5. Connect the Promiscuous Pipelines
      // Anything hitting the VLAN goes to space; anything hitting space goes to the VLAN
      g_fdDev[i]->SetPromiscReceiveCallback (MakeCallback (&Forward_Fd_to_Space));
      g_spaceDev[i]->SetPromiscReceiveCallback (MakeCallback (&Forward_Space_to_Fd));
    }

  emuHelper.EnablePcapAll("satellite-link-emulation-any2any", true);

  // Run parameters
  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("ns-3 4-Node ANY-TO-ANY Bus Pipeline Active");
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}