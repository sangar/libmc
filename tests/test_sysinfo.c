#include "test.h"

#include "mc/platform/platform.h"
#include "mc/platform/pty.h"
#include "mc/platform/sysinfo.h"

static void system_sample_reads_the_machine(Test *test)
{
    Err err = { 0 };
    SystemSample first;
    SystemSample second;
    test_check(test, system_sample(test->arena, &first, &err) == ERR_OK, "sample once");
    clock_sleep_ms(50);
    test_check(test, system_sample(test->arena, &second, &err) == ERR_OK, "sample again");
    test_check(test, first.core_count > 0 && first.core_count == thread_cpu_count(), "one entry per core");
    test_check(test, first.cpu.busy <= first.cpu.total && second.cpu.total >= first.cpu.total, "ticks only grow");
    test_check(test, first.memory_used_bytes > 0 && first.memory_used_bytes < first.memory_total_bytes,
               "some of the memory is used");
    test_check(test, first.uptime_seconds > 0, "the machine has been up");
}

static void process_groups_sample_adds_up_each_group(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    const char *const argv[] = { "sh", "-c", "sleep 30 & sleep 30 & while :; do :; done" };
    Pty pty;
    test_check(test, pty_spawn(a, strlist_of(a, countof(argv), argv), S(""), (StringList) { 0 },
                               (PtySize) { .columns = 80, .rows = 24 }, &pty, &err) == ERR_OK,
               "spawn a busy group");
    GroupSample samples[2] = { 0 };
    const int groups[] = { pty.pid, 0x7ffffff0 };
    GroupSample sample = { 0 };
    int64_t deadline = clock_monotonic_ns() + 5 * (int64_t)NS_PER_SECOND;
    while ((sample.process_count < 3 || sample.cpu_ns < 20 * (uint64_t)NS_PER_MILLISECOND) &&
           clock_monotonic_ns() < deadline) {
        clock_sleep_ms(20);
        Error sampled = process_groups_sample(a, groups, countof(groups), samples, &err);
        test_check(test, sampled == ERR_OK, "sample the groups");
        sample = samples[0];
    }
    test_check(test, sample.process_count == 3, "the shell and its two sleeps");
    test_check(test, sample.thread_count >= 3 && sample.resident_bytes > 0, "threads and memory");
    test_check(test, sample.cpu_ns >= 20 * (uint64_t)NS_PER_MILLISECOND, "the busy loop uses CPU time");
    test_check(test, samples[1].process_count == 0 && samples[1].cpu_ns == 0, "a group without processes");

    test_check(test, pty_signal(&pty, PTY_KILL, &err) == ERR_OK, "kill the group");
    int exit_code;
    int signal;
    while (!pty_reap(&pty, &exit_code, &signal) && clock_monotonic_ns() < deadline) {
        clock_sleep_ms(10);
    }
    pty_close(&pty);
}

const TestCase SYSINFO_TESTS[] = {
    { "system_sample_reads_the_machine", system_sample_reads_the_machine },
    { "process_groups_sample_adds_up_each_group", process_groups_sample_adds_up_each_group },
    { nullptr, nullptr },
};
