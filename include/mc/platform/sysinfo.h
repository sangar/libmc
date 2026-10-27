#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// What a system monitor shows: how busy the machine is, how full its memory
// is, how much its network moves, and what a group of processes costs. Every
// counter that grows with time is cumulative, so a rate is the difference of
// two samples over the time between them.

enum { SYSINFO_MAX_CORES = 128 };

// CpuTicks counts time in arbitrary units that are equal across the fields
// of one sample: busy is what ran work, total is busy plus idle. The share of
// the time a core was busy between two samples is (busy2 - busy1) / (total2 - total1).
typedef struct CpuTicks {
    uint64_t busy;
    uint64_t total;
} CpuTicks;

// cpu_busy_share is the share of the time between two samples a core was
// busy, 0 to 1; 0 when the counters went backwards or did not move.
float cpu_busy_share(CpuTicks before, CpuTicks after);
// counter_rate is how much a cumulative counter grew per second; 0 when it
// went backwards, as after a reset.
double counter_rate(uint64_t before, uint64_t after, double seconds);

typedef struct SystemSample {
    size_t core_count;
    // cpu is every core together; cores holds the first SYSINFO_MAX_CORES.
    CpuTicks cpu;
    CpuTicks cores[SYSINFO_MAX_CORES];
    // memory_used_bytes is what programs and the kernel hold, not counting
    // caches the system gives back on demand.
    uint64_t memory_total_bytes;
    uint64_t memory_used_bytes;
    uint64_t swap_total_bytes;
    uint64_t swap_used_bytes;
    // network counts the bytes of every interface but loopback.
    uint64_t network_received_bytes;
    uint64_t network_sent_bytes;
    double load_average[3]; // over 1, 5 and 15 minutes
    int64_t uptime_seconds;
} SystemSample;

// system_sample reads the counters of the whole machine. The text it reads on
// the way is allocated in scratch.
[[nodiscard]] Error system_sample(Arena *scratch, SystemSample *sample, Err *err);

typedef struct GroupSample {
    size_t process_count;
    size_t thread_count;
    // cpu_ns is the CPU time every process now in the group has used since it
    // started, so it drops when a process leaves.
    uint64_t cpu_ns;
    uint64_t resident_bytes;
} GroupSample;

// process_groups_sample fills samples[i] with the sum over the live processes
// of group pgids[i]; a group without processes is all zeroes. One pass over
// the system's processes serves every group. The list of processes is
// allocated in scratch.
[[nodiscard]] Error process_groups_sample(Arena *scratch, const int *pgids, size_t count, GroupSample *samples,
                                          Err *err);
