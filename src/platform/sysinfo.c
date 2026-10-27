#include "mc/platform/sysinfo.h"

float cpu_busy_share(CpuTicks before, CpuTicks after)
{
    if (after.total <= before.total || after.busy < before.busy) {
        return 0.0f;
    }
    float share = (float)(after.busy - before.busy) / (float)(after.total - before.total);
    return share > 1.0f ? 1.0f : share;
}

double counter_rate(uint64_t before, uint64_t after, double seconds)
{
    return after >= before && seconds > 0.0 ? (double)(after - before) / seconds : 0.0;
}
