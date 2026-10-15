#ifdef __APPLE__

#include <errno.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <net/if.h>
#include <net/route.h>
#include <stdlib.h>
#include <sys/sysctl.h>
#include <sys/time.h>

// Cross builds use zig's macOS headers, which leave out net/if_types.h; the
// interface types are a fixed part of the routing socket ABI.
#if __has_include(<net/if_types.h>)
#include <net/if_types.h>
#else
enum { IFT_LOOP = 0x18 };
#endif

#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/platform/sysinfo.h"
#include "posix_shared.h"


static void read_cores(SystemSample *sample)
{
    natural_t cores = 0;
    processor_info_array_t info = nullptr;
    mach_msg_type_number_t info_count = 0;
    if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, &cores, &info, &info_count) != KERN_SUCCESS) {
        return;
    }
    sample->core_count = cores;
    for (natural_t i = 0; i < cores; i++) {
        const integer_t *ticks = info + (size_t)i * CPU_STATE_MAX;
        uint64_t busy =
            (uint64_t)ticks[CPU_STATE_USER] + (uint64_t)ticks[CPU_STATE_SYSTEM] + (uint64_t)ticks[CPU_STATE_NICE];
        CpuTicks core = { .busy = busy, .total = busy + (uint64_t)ticks[CPU_STATE_IDLE] };
        sample->cpu.busy += core.busy;
        sample->cpu.total += core.total;
        if (i < SYSINFO_MAX_CORES) {
            sample->cores[i] = core;
        }
    }
    vm_deallocate(mach_task_self(), (vm_address_t)info, info_count * sizeof(integer_t));
}

static void read_memory(SystemSample *sample)
{
    uint64_t total = 0;
    size_t size = sizeof total;
    sysctlbyname("hw.memsize", &total, &size, nullptr, 0);
    sample->memory_total_bytes = total;

    vm_size_t page_size = 0;
    host_page_size(mach_host_self(), &page_size);
    vm_statistics64_data_t vm = { 0 };
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        // What Activity Monitor calls memory used: app memory, wired and compressed.
        uint64_t app_pages =
            vm.internal_page_count > vm.purgeable_count ? vm.internal_page_count - vm.purgeable_count : 0;
        sample->memory_used_bytes = (app_pages + vm.wire_count + vm.compressor_page_count) * page_size;
    }

    struct xsw_usage swap = { 0 };
    size = sizeof swap;
    if (sysctlbyname("vm.swapusage", &swap, &size, nullptr, 0) == 0) {
        sample->swap_total_bytes = swap.xsu_total;
        sample->swap_used_bytes = swap.xsu_used;
    }
}

enum { INTERFACE_LIST_TRIES = 4 };

// read_network adds up the counters of every interface but loopback, up or
// down, so that an interface going down for a while does not make the total
// drop and then jump back. It fails when it cannot list the interfaces.
[[nodiscard]] static bool read_network(Arena *scratch, SystemSample *sample)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, 0, NET_RT_IFLIST2, 0 };
    ArenaMark mark = arena_mark(scratch);
    char *list = nullptr;
    size_t length = 0;
    // The list can grow between asking for its length and reading it.
    bool listed = false;
    for (int tries = 0; !listed && tries < INTERFACE_LIST_TRIES; tries++) {
        if (sysctl(mib, 6, nullptr, &length, nullptr, 0) != 0) {
            break;
        }
        length += length / 8;
        list = arena_push(scratch, length);
        listed = sysctl(mib, 6, list, &length, nullptr, 0) == 0;
        if (!listed && errno != ENOMEM) {
            break;
        }
    }
    for (size_t at = 0; listed && at + sizeof(struct if_msghdr) <= length;) {
        const struct if_msghdr *header = (const struct if_msghdr *)(list + at);
        if (header->ifm_msglen == 0) {
            break;
        }
        if (header->ifm_type == RTM_IFINFO2) {
            const struct if_msghdr2 *info = (const struct if_msghdr2 *)header;
            if (info->ifm_data.ifi_type != IFT_LOOP) {
                sample->network_received_bytes += info->ifm_data.ifi_ibytes;
                sample->network_sent_bytes += info->ifm_data.ifi_obytes;
            }
        }
        at += header->ifm_msglen;
    }
    arena_release(mark);
    return listed;
}

Error system_sample(Arena *scratch, SystemSample *sample, Err *err)
{
    *sample = (SystemSample) { 0 };
    read_cores(sample);
    read_memory(sample);
    if (!read_network(scratch, sample)) {
        return posix_fail_errno(err, "list network interfaces");
    }
    if (getloadavg(sample->load_average, 3) < 0) {
        return posix_fail_errno(err, "read load average");
    }
    struct timeval boot;
    size_t size = sizeof boot;
    if (sysctlbyname("kern.boottime", &boot, &size, nullptr, 0) == 0) {
        sample->uptime_seconds = clock_wall_ns() / NS_PER_SECOND - boot.tv_sec;
    }
    return ERR_OK;
}

// The task counters are in units of the Mach clock, which is not nanoseconds on Apple silicon.
static uint64_t mach_to_ns(uint64_t ticks)
{
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    return ticks * timebase.numer / timebase.denom;
}

static void sample_group(Arena *scratch, int pgid, GroupSample *sample)
{
    *sample = (GroupSample) { 0 };
    int bytes = proc_listpids(PROC_PGRP_ONLY, (uint32_t)pgid, nullptr, 0);
    if (bytes <= 0) {
        return;
    }
    ArenaMark mark = arena_mark(scratch);
    // A few spare slots: processes can start between the two calls.
    size_t capacity = (size_t)bytes / sizeof(pid_t) + 16;
    pid_t *pids = arena_push(scratch, arena_size_mul(capacity, sizeof *pids));
    bytes = proc_listpids(PROC_PGRP_ONLY, (uint32_t)pgid, pids, (int)(capacity * sizeof *pids));
    uint64_t ticks = 0;
    for (size_t i = 0; bytes > 0 && i < (size_t)bytes / sizeof(pid_t); i++) {
        struct proc_taskinfo task;
        if (pids[i] == 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &task, sizeof task) != (int)sizeof task) {
            continue;
        }
        sample->process_count++;
        sample->thread_count += (size_t)task.pti_threadnum;
        sample->resident_bytes += task.pti_resident_size;
        ticks += task.pti_total_user + task.pti_total_system;
    }
    sample->cpu_ns = mach_to_ns(ticks);
    arena_release(mark);
}

Error process_groups_sample(Arena *scratch, const int *pgids, size_t count, GroupSample *samples, Err *err)
{
    for (size_t i = 0; i < count; i++) {
        sample_group(scratch, pgids[i], &samples[i]);
    }
    unused(err);
    return ERR_OK;
}

#endif
