// The reachability flags c-ares's src/lib/ares_sysconfig_mac.c tests, for
// cross builds without the macOS SDK; see SCDynamicStoreCopySpecific.h.
#pragma once

enum {
    kSCNetworkFlagsReachable = 1 << 1,
    kSCNetworkReachabilityFlagsConnectionOnTraffic = 1 << 3,
};
