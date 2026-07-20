import sys
import os
from ns import ns

def parse_trace(filename):
    """Parses topology_trace.txt file structure into a timeline dictionary"""
    trace_data = {}
    current_time = None
    
    with open(filename, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('TIMESTAMP'):
                current_time = float(line.split()[1])
                trace_data[current_time] = []
            elif line == 'END':
                current_time = None
            else:
                if current_time is not None:
                    parts = line.split()
                    src = int(parts[0])
                    dst = int(parts[1])
                    bw = float(parts[2])
                    loss = float(parts[3])
                    trace_data[current_time].append((src, dst, bw, loss))
    return trace_data

# Global tracking map for mesh links
# Key: (min(src,dst), max(src,dst)) -> Value: (dev_src, dev_dst, qd_src, qd_dst)
link_devices = {}

def change_link_properties(src, dst, bw_mbps, loss_rate):
    """Dynamically scales CSMA link attributes using Traffic Control and Error Models"""
    key = (min(src, dst), max(src, dst))
    if key in link_devices:
        dev_src, dev_dst, qd_src, qd_dst = link_devices[key]
        
        # 1. Update Bandwidth dynamically via the TBF Queue Disc
        new_bw = ns.DataRate(f"{bw_mbps}Mbps")
        qd_src.SetAttribute("Rate", ns.DataRateValue(new_bw))
        qd_dst.SetAttribute("Rate", ns.DataRateValue(new_bw))
        
        # 2. Update Loss Rate dynamically by swapping the error model
        error_model = ns.CreateObject("RateErrorModel")
        error_model.SetAttribute("ErrorUnit", ns.EnumValue(ns.RateErrorModel.ERROR_UNIT_PACKET))
        error_model.SetAttribute("ErrorRate", ns.DoubleValue(loss_rate))
        
        # Attach the random packet drops to the receive paths
        dev_src.SetReceiveErrorModel(error_model)
        dev_dst.SetReceiveErrorModel(error_model)
        
        print(f"[Time {ns.Simulator.Now().GetSeconds():.2f}s] Updated link ({src}<->{dst}): BW={bw_mbps}Mbps, Drop Rate={loss_rate}")

def main():
    # Use absolute path to ensure sudo finds it
    trace_file = "/home/ijoldenb/ns-3.48/scratch/topology_trace.txt"
    try:
        topology_schedule = parse_trace(trace_file)
    except FileNotFoundError:
        print(f"Error: {trace_file} not found.")
        return

    num_pi = 4  # Start with 4, scale to 20 later
    
    nodes = ns.NodeContainer()
    nodes.Create(num_pi)
    
    # Disable IPv6 inside ns-3 to prevent internal broadcast storms from Pi's
    stack = ns.InternetStackHelper()
    stack.SetIpv6StackInstall(False)
    stack.Install(nodes)
    
    # 1. Setup Static CSMA Topology with TBF Traffic Control
    csma = ns.CsmaHelper()
    tc = ns.TrafficControlHelper()
    ip_helper = ns.Ipv4AddressHelper()
    
    # --- FIX: Move SetRootQueueDisc OUTSIDE the loop ---
    # Configure the Token Bucket Filter (TBF) blueprint once
    tc.SetRootQueueDisc("ns3::TbfQueueDisc",
                        "Rate", ns.StringValue("10Mbps"), 
                        "Burst", ns.UintegerValue(65535), 
                        "Mtu", ns.UintegerValue(1500))
    
    subnet_idx = 1
    for i in range(num_pi):
        for j in range(i + 1, num_pi):
            # Physical wire is super fast (1 Gbps); we will bottleneck it in software
            csma.SetChannelAttribute("DataRate", ns.StringValue("1Gbps"))
            csma.SetChannelAttribute("Delay", ns.StringValue("0ms"))
            
            # Put the two target nodes into a temporary NodeContainer
            link_nodes = ns.NodeContainer()
            link_nodes.Add(nodes.Get(i))
            link_nodes.Add(nodes.Get(j))
            
            # Install CSMA over the 2-node container
            link_devs = csma.Install(link_nodes)
            
            dev_i = link_devs.Get(0)
            dev_j = link_devs.Get(1)
            
            # --- FIX: Now just call Install() inside the loop ---
            # It will safely use the blueprint we defined above
            tc_devs_i = tc.Install(dev_i)
            tc_devs_j = tc.Install(dev_j)
            
            qd_i = tc_devs_i.Get(0)
            qd_j = tc_devs_j.Get(0)
            
            # Store the devices AND the queue discs in our tracker mapping
            link_devices[(i, j)] = (dev_i, dev_j, qd_i, qd_j)
            
            # Subnetting for internal routing between nodes
            ip_helper.SetBase(ns.Ipv4Address(f"172.16.{subnet_idx}.0"), ns.Ipv4Mask("255.255.255.0"))
            ip_helper.Assign(link_devs)
            subnet_idx += 1

    # 2. Attach EmuFdNetDevices to hook external physical Pi's to ns-3 nodes
    emu_helper = ns.EmuFdNetDeviceHelper()
    
    for i in range(num_pi):
        # --- FIX: Match your live Linux naming scheme ---
        # i=0 -> vlan101, i=1 -> vlan102, i=2 -> vlan103, i=3 -> vlan104
        vlan_interface = f"vlan{101 + i}"
        
        # Set the matching device name
        emu_helper.SetDeviceName(vlan_interface)
        
        emu_devices = emu_helper.Install(nodes.Get(i))
        
        # Keep the subnets aligned with your custom VLAN IDs
        ip_helper.SetBase(ns.Ipv4Address(f"10.0.{101 + i}.0"), ns.Ipv4Mask("255.255.255.0"))
        ip_helper.Assign(emu_devices)

    # Populate global routing tables so ns-3 knows how to route across the internal links
    ns.Ipv4GlobalRoutingHelper.PopulateRoutingTables()

    # 3. Schedule Dynamic Attribute Modifications based on Timeline Map
    for timestamp, alterations in topology_schedule.items():
        for alteration in alterations:
            src, dst, bw, loss = alteration
            
            # --- FIX: Wrap the function and its arguments in a lambda ---
            ns.Simulator.Schedule(
                ns.Seconds(timestamp), 
                lambda s=src, d=dst, b=bw, l=loss: change_link_properties(s, d, b, l)
            )
    print("--- Booting Python ns-3 Engine (CSMA Emulation + Traffic Control) ---")
    ns.Simulator.Stop(ns.Seconds(3500))  
    ns.Simulator.Run()
    ns.Simulator.Destroy()
    print("Simulation stopped cleanly.")

if __name__ == '__main__':
    main()