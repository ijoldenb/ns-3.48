import sys
import os

# 1. Force Python to look into your ns-3 build directories
ns3_build_lib = "/home/ijoldenb/ns-3.48/build/lib"
ns3_bindings = "/home/ijoldenb/ns-3.48/build/bindings/python"

if ns3_build_lib not in sys.path:
    sys.path.insert(0, ns3_build_lib)
if ns3_bindings not in sys.path:
    sys.path.insert(0, ns3_bindings)

# 2. Ensure the system link-loader can see the raw C++ .so shared objects under sudo
os.environ["LD_LIBRARY_PATH"] = ns3_build_lib + (f":{os.environ.get('LD_LIBRARY_PATH', '')}" if os.environ.get('LD_LIBRARY_PATH') else "")

# 3. Now perform the sub-module imports safely
import ns.core
import ns.network
import ns.internet
import ns.point_to_point
import ns.fd_net_device

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
link_devices = {}

def change_link_properties(src, dst, bw_mbps, loss_rate):
    """Callback function that dynamically scales specific PointToPoint link attributes"""
    key = (min(src, dst), max(src, dst))
    if key in link_devices:
        dev_src, dev_dst = link_devices[key]
        
        # Update link speed symmetrically
        new_bw = ns.core.DataRate(f"{bw_mbps}Mbps")
        dev_src.SetAttribute("DataRate", ns.core.DataRateValue(new_bw))
        dev_dst.SetAttribute("DataRate", ns.core.DataRateValue(new_bw))
        
        # Instantiate a packet loss model based on drop percentage
        error_model = ns.core.CreateObject("RateErrorModel")
        error_model.SetAttribute("ErrorUnit", ns.core.EnumValue(ns.network.RateErrorModel.ERROR_UNIT_PACKET))
        error_model.SetAttribute("ErrorRate", ns.core.DoubleValue(loss_rate))
        
        # Attach the drops to the receive paths of both endpoints
        dev_src.SetReceiveErrorModel(error_model)
        dev_dst.SetReceiveErrorModel(error_model)
        
        print(f"[Time {ns.core.Simulator.Now().GetSeconds()}s] Applied properties to mesh ({src}<->{dst}): BW={bw_mbps}Mbps, Drop Rate={loss_rate}")

def main():
    # Provide the absolute path to ensure sudo finds it regardless of current working directory
    trace_file = "/home/ijoldenb/ns-3.48/topology_trace.txt"
    try:
        topology_schedule = parse_trace(trace_file)
    except FileNotFoundError:
        print(f"Error: {trace_file} not found.")
        return

    num_pi = 4  
    
    nodes = ns.network.NodeContainer()
    nodes.Create(num_pi)
    
    stack = ns.internet.InternetStackHelper()
    stack.Install(nodes)
    
    # 1. Setup Static Point-to-Point Mesh Topology
    p2p = ns.point_to_point.PointToPointHelper()
    ip_helper = ns.internet.Ipv4AddressHelper()
    
    subnet_idx = 1
    for i in range(num_pi):
        for j in range(i + 1, num_pi):
            p2p.SetDeviceAttribute("DataRate", ns.core.StringValue("10Mbps"))
            p2p.SetChannelAttribute("Delay", ns.core.StringValue("1ms"))
            
            link_devs = p2p.Install(nodes.Get(i), nodes.Get(j))
            
            dev_i = ns.point_to_point.PointToPointNetDevice.ConvertFrom(link_devs.Get(0))
            dev_j = ns.point_to_point.PointToPointNetDevice.ConvertFrom(link_devs.Get(1))
            link_devices[(i, j)] = (dev_i, dev_j)
            
            ip_helper.SetBase(ns.network.Ipv4Address(f"172.16.{subnet_idx}.0"), ns.network.Ipv4Mask("255.255.255.0"))
            ip_helper.Assign(link_devs)
            subnet_idx += 1

    # 2. Attach EmuFdNetDevices to connect external physical Pi's
    emu_helper = ns.fd_net_device.FdNetDeviceHelper()
    emu_helper.SetType("ns3::EmuFdNetDevice")
    
    for i in range(num_pi):
        vlan_interface = f"eth0.{10 * (i + 1)}"
        emu_helper.SetAttribute("DeviceName", ns.core.StringValue(vlan_interface))
        
        emu_devices = emu_helper.Install(nodes.Get(i))
        emu_dev = emu_devices.Get(0)
        
        emu_dev.SetAttribute("Promiscuous", ns.core.BooleanValue(True))
        
        ip_helper.SetBase(ns.network.Ipv4Address(f"10.0.{10 * (i + 1)}.0"), ns.network.Ipv4Mask("255.255.255.0"))
        ip_helper.Assign(emu_devices)

    ns.internet.Ipv4GlobalRoutingHelper.PopulateRoutingTables()

    # 3. Schedule Dynamic Attribute Modifications based on Timeline Map
    for timestamp, alterations in topology_schedule.items():
        for alteration in alterations:
            src, dst, bw, loss = alteration
            ns.core.Simulator.Schedule(ns.core.Seconds(timestamp), change_link_properties, src, dst, bw, loss)

    print("--- Booting Python-bound ns-3 Engine with EmuFdNetDevice Mesh ---")
    ns.core.Simulator.Stop(ns.core.Seconds(3500))  
    ns.core.Simulator.Run()
    ns.core.Simulator.Destroy()
    print("Simulation stopped cleanly.")

if __name__ == '__main__':
    main()