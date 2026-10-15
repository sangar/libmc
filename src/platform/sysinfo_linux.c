#ifdef __linux__
#define _GNU_SOURCE

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "mc/core/arena.h"
#include "mc/platform/platform.h"
#include "mc/platform/sysinfo.h"

// Everything here comes from procfs, whose files are text read whole.

// field_u64 is the index-th whitespace-separated field of line as a number, 0 when absent.
static uint64_t field_u64(StringList fields, size_t index)
{
    uint64_t value = 0;
    return index < fields.count && str_parse_u64(fields.items[index], &value) ? value : 0;
}

static void read_cpu(Arena *scratch, SystemSample *sample, Err *err)
{
    String text;
    if (file_read_all(scratch, S("/proc/stat"), &text, err) != ERR_OK) {
        return;
    }
    StringList lines = strlist_from_lines(scratch, text);
    for (size_t i = 0; i < lines.count; i++) {
        if (!str_starts_with(lines.items[i], S("cpu"))) {
            continue;
        }
        // cpu user nice system idle iowait irq softirq steal
        StringList fields = str_fields(scratch, lines.items[i]);
        uint64_t busy = field_u64(fields, 1) + field_u64(fields, 2) + field_u64(fields, 3) + field_u64(fields, 6) +
                        field_u64(fields, 7) + field_u64(fields, 8);
        CpuTicks ticks = { .busy = busy, .total = busy + field_u64(fields, 4) + field_u64(fields, 5) };
        if (str_equal(fields.items[0], S("cpu"))) {
            sample->cpu = ticks;
            continue;
        }
        if (sample->core_count < SYSINFO_MAX_CORES) {
            sample->cores[sample->core_count] = ticks;
        }
        sample->core_count++;
    }
}

static void read_memory(Arena *scratch, SystemSample *sample, Err *err)
{
    String text;
    if (file_read_all(scratch, S("/proc/meminfo"), &text, err) != ERR_OK) {
        return;
    }
    uint64_t total = 0;
    uint64_t available = 0;
    uint64_t swap_total = 0;
    uint64_t swap_free = 0;
    StringList lines = strlist_from_lines(scratch, text);
    for (size_t i = 0; i < lines.count; i++) {
        StringList fields = str_fields(scratch, lines.items[i]);
        if (fields.count < 2) {
            continue;
        }
        uint64_t kilobytes = field_u64(fields, 1);
        String name = fields.items[0];
        if (str_equal(name, S("MemTotal:"))) {
            total = kilobytes;
        } else if (str_equal(name, S("MemAvailable:"))) {
            available = kilobytes;
        } else if (str_equal(name, S("SwapTotal:"))) {
            swap_total = kilobytes;
        } else if (str_equal(name, S("SwapFree:"))) {
            swap_free = kilobytes;
        }
    }
    sample->memory_total_bytes = total * 1024;
    sample->memory_used_bytes = (total > available ? total - available : 0) * 1024;
    sample->swap_total_bytes = swap_total * 1024;
    sample->swap_used_bytes = (swap_total > swap_free ? swap_total - swap_free : 0) * 1024;
}

static void read_network(Arena *scratch, SystemSample *sample, Err *err)
{
    String text;
    if (file_read_all(scratch, S("/proc/net/dev"), &text, err) != ERR_OK) {
        return;
    }
    // Two header lines, then "name: rx_bytes 7 more receive fields tx_bytes ...".
    StringList lines = strlist_from_lines(scratch, text);
    for (size_t i = 2; i < lines.count; i++) {
        String name;
        String counters;
        if (!str_cut(lines.items[i], ':', &name, &counters) || str_equal(str_trim(name), S("lo"))) {
            continue;
        }
        StringList fields = str_fields(scratch, counters);
        sample->network_received_bytes += field_u64(fields, 0);
        sample->network_sent_bytes += field_u64(fields, 8);
    }
}

Error system_sample(Arena *scratch, SystemSample *sample, Err *err)
{
    *sample = (SystemSample) { 0 };
    ArenaMark mark = arena_mark(scratch);
    read_cpu(scratch, sample, err);
    read_memory(scratch, sample, err);
    read_network(scratch, sample, err);
    arena_release(mark);
    if (sample->cpu.total == 0) {
        return err_set(err, ERR_PLATFORM, "read /proc/stat: no cpu line");
    }
    if (getloadavg(sample->load_average, 3) < 0) {
        return err_set(err, ERR_PLATFORM, "read load average");
    }
    struct timespec boot;
    clock_gettime(CLOCK_BOOTTIME, &boot);
    sample->uptime_seconds = boot.tv_sec;
    return ERR_OK;
}

// add_process adds /proc/<pid>/stat to the sample of its group when that is one of pgids.
static void add_process(Arena *scratch, String pid, const int *pgids, size_t count, GroupSample *samples)
{
    String text;
    String path = str_format(scratch, "/proc/%.*s/stat", (int)pid.len, pid.data);
    size_t paren;
    if (file_read_all(scratch, path, &text, nullptr) != ERR_OK || !str_find_last_char(text, ')', &paren)) {
        return;
    }
    // After "pid (comm) ": state ppid pgrp session tty_nr tpgid flags minflt
    // cminflt majflt cmajflt utime stime cutime cstime priority nice
    // num_threads itrealvalue starttime vsize rss
    StringList fields = str_fields(scratch, str_slice(text, paren + 1, text.len));
    int64_t group = 0;
    if (fields.count < 22 || !str_parse_i64(fields.items[2], &group)) {
        return;
    }
    for (size_t i = 0; i < count; i++) {
        if (pgids[i] != group) {
            continue;
        }
        uint64_t ticks_per_second = (uint64_t)sysconf(_SC_CLK_TCK);
        uint64_t page_size = (uint64_t)sysconf(_SC_PAGESIZE);
        GroupSample *sample = &samples[i];
        sample->process_count++;
        sample->cpu_ns += (field_u64(fields, 11) + field_u64(fields, 12)) * NS_PER_SECOND / ticks_per_second;
        sample->thread_count += (size_t)field_u64(fields, 17);
        sample->resident_bytes += field_u64(fields, 21) * page_size;
    }
}

Error process_groups_sample(Arena *scratch, const int *pgids, size_t count, GroupSample *samples, Err *err)
{
    for (size_t i = 0; i < count; i++) {
        samples[i] = (GroupSample) { 0 };
    }
    if (count == 0) {
        return ERR_OK;
    }
    ArenaMark mark = arena_mark(scratch);
    StringList names = { 0 };
    Error listed = dir_list(scratch, S("/proc"), &names, err);
    for (size_t i = 0; listed == ERR_OK && i < names.count; i++) {
        if (str_is_digits(names.items[i])) {
            ArenaMark per_process = arena_mark(scratch);
            add_process(scratch, names.items[i], pgids, count, samples);
            arena_release(per_process);
        }
    }
    arena_release(mark);
    return listed;
}

#endif
