#pragma once

#include "mc/text/str.h"

// fmt_duration and fmt_parse_duration use Go's notation: 750ms, 1.5s, 10m0s, 1h0m0s.
String fmt_duration(Arena *arena, int64_t nanoseconds);
// fmt_parse_duration accepts the whole int64_t range and truncates a fraction
// of a nanosecond toward zero.
bool fmt_parse_duration(String text, int64_t *nanoseconds);

// fmt_rfc3339 writes a Unix time as 2024-03-13T10:00:00+01:00 in the zone
// utc_offset seconds east of UTC, with milliseconds when asked.
String fmt_rfc3339(Arena *arena, int64_t unix_nanoseconds, int32_t utc_offset, bool milliseconds);
// fmt_thousands writes 1234567 as "1,234,567".
String fmt_thousands(Arena *arena, int64_t value);
// fmt_bytes writes a byte count with a binary unit: "512 B", "1.5 MB".
String fmt_bytes(Arena *arena, int64_t bytes);
