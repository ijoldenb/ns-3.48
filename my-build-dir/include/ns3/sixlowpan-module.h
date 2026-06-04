#ifdef NS3_MODULE_COMPILATION 
    error "Do not include ns3 module aggregator headers from other modules these are meant only for end user scripts." 
#endif 
#ifndef NS3_MODULE_SIXLOWPAN
    // Module headers: 
    #include <ns3/sixlowpan-helper.h>
    #include <ns3/sixlowpan-header.h>
    #include <ns3/sixlowpan-nd-context.h>
    #include <ns3/sixlowpan-nd-header.h>
    #include <ns3/sixlowpan-nd-prefix.h>
    #include <ns3/sixlowpan-nd-protocol.h>
    #include <ns3/sixlowpan-net-device.h>
    #include <ns3/sixlowpan-nd-binding-table.h>
#endif 