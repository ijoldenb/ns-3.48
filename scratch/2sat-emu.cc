#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/fd-net-device-module.h"

using namespace ns3;

void KeepAliveDummyEvent ()
{
    // This function intentionally left blank. It serves as a "keep-alive" event to ensure the simulation runs until the specified stop time.
}

int main (int argc, char *argv[])
{
  CommandLine cmd;
  cmd.Parse (argc, argv);

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));
  
  // CHANGE THIS STRING to match your exact VirtualBox interface (e.g., "eth0", "enp0s3", "enp0s8")
  std::string physicalInterface = "enp0s8"; 

  // 1. Create the internal emulation topology nodes
  NodeContainer nodes;
  nodes.Create (2);

  // 2. Define the Satellite Physical Channel constraints
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("5Mbps"));     // Bandwidth Cap
  p2p.SetChannelAttribute ("Delay", StringValue ("120ms"));       // One-way Satellite Latency

  // 3. Connect the nodes internally
  NetDeviceContainer internalDevs = p2p.Install (nodes);

  // 4. Bind the File Descriptor engine to the physical network card
  EmuFdNetDeviceHelper emuHelper;
  emuHelper.SetDeviceName (physicalInterface); 
  
  // Connect Simulation Node 0 to the physical network (Handles Pi 1)
  NetDeviceContainer emuDevs1 = emuHelper.Install (nodes.Get (0));
  Ptr<FdNetDevice> emuDev1 = emuDevs1.Get (0)->GetObject<FdNetDevice> ();
  
  // Connect Simulation Node 1 to the physical network (Handles Pi 2)
  NetDeviceContainer emuDevs2 = emuHelper.Install (nodes.Get (1));
  Ptr<FdNetDevice> emuDev2 = emuDevs2.Get (0)->GetObject<FdNetDevice> ();

  // 6. Data Capture telemetry loop
  p2p.EnablePcapAll ("satellite-emu-flight");

  Time stopTime = Seconds (6000.0); 
  Simulator::Stop (stopTime);


  // Correct syntax: Schedule our empty function right before the simulation ends
  Simulator::Schedule (stopTime - Seconds(1.0), &KeepAliveDummyEvent);

  NS_LOG_UNCOND ("================================================================");
  NS_LOG_UNCOND ("Simulation Engine Active. Bound strictly to device: " << physicalInterface);
  NS_LOG_UNCOND ("================================================================");

  Simulator::Run ();
  Simulator::Destroy ();
  
  return 0;
}