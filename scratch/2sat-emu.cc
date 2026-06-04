#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/internet-module.h"
#include "ns3/fd-net-device-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/flow-monitor-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("SatelliteHardwareInTheLoop");

int main (int argc, char *argv[])
{
  // 1. Define tunable network restrictions (Change these to alter your orbit metrics)
  std::string bandwidth = "5Mbps";    // Max Bandwidth ceiling
  std::string delay     = "120ms";    // Simulated propagation delay (e.g., LEO orbit distance)
  double simulationTime = 30.0;       // Duration of test in seconds

  CommandLine cmd (__FILE__);
  cmd.AddValue ("bandwidth", "Max data rate ceiling", bandwidth);
  cmd.AddValue ("delay", "Simulated satellite link delay", delay);
  cmd.Parse (argc, argv);

  // 2. Turn on the Real-Time Simulator Clock (Crucial for hardware synchronization)
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));

  NS_LOG_UNCOND ("Initializing Emulation Loop... Bound to enp0s8.101 and enp0s8.102");

  // 3. Create the inner-simulation representations of your two physical Pis
  NodeContainer nodes;
  nodes.Create (2); // Node 0 = Pi 1, Node 1 = Pi 2

  // 4. Create the link channel that manipulates latency and bandwidth ceilings
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue (bandwidth));
  p2p.SetChannelAttribute ("Delay", StringValue (delay));
  
  // Use a bounded internal queue so packets drop naturally if the Pis exceed maximum bandwidth
  p2p.SetQueue ("ns3::DropTailQueue", "MaxSize", StringValue ("50p")); 
  NetDeviceContainer internalDevices = p2p.Install (nodes);

  // 5. Establish Emu File Descriptor mappings to catch physical raw packets
  EmuFdNetDeviceHelper emuHelper;

  // Bind Node 0 to physical Pi 1's trunked interface
  emuHelper.SetAttribute ("DeviceName", StringValue ("enp0s8.101"));
  NetDeviceContainer emuDevs1 = emuHelper.Install (nodes.Get (0));
  
  // Bind Node 1 to physical Pi 2's trunked interface
  emuHelper.SetAttribute ("DeviceName", StringValue ("enp0s8.102"));
  NetDeviceContainer emuDevs2 = emuHelper.Install (nodes.Get (1));

  // 6. Hook up FlowMonitor to gather throughput, delay, and packet loss metrics
  FlowMonitorHelper flowmon;
  Ptr<FlowMonitor> monitor = flowmon.InstallAll();

  // 8. Extract and display performance data when the script completes
  monitor->CheckForLostPackets ();
  Ptr<Ipv4FlowClassifier> classifier = DynamicCast<Ipv4FlowClassifier> (flowmon.GetClassifier ());
  std::map<FlowId, FlowMonitor::FlowStats> stats = monitor->GetFlowStats ();

  NS_LOG_UNCOND ("\n--- EMULATION RESULTS ---");
  for (std::map<FlowId, FlowMonitor::FlowStats>::const_iterator i = stats.begin (); i != stats.end (); ++i)
    {
      Ipv4FlowClassifier::FiveTuple t = classifier->FindFlow (i->first);
      NS_LOG_UNCOND ("Flow " << i->first << " (" << t.sourceAddress << " -> " << t.destinationAddress << ")");
      NS_LOG_UNCOND ("  Tx Packets: " << i->second.txPackets);
      NS_LOG_UNCOND ("  Rx Packets: " << i->second.rxPackets);
      NS_LOG_UNCOND ("  Lost Packets: " << i->second.lostPackets);
      NS_LOG_UNCOND ("  Mean Latency: " << i->second.delaySum.GetMilliSeconds() / i->second.rxPackets << " ms");
      NS_LOG_UNCOND ("  Max Throughput Achieved: " << (i->second.rxBytes * 8.0) / (simulationTime * 1000000.0) << " Mbps");
    }
    
  // 7. Execute the simulation loop
  Simulator::Stop (Seconds (simulationTime));
  Simulator::Run ();
  Simulator::Destroy ();
  return 0;

}