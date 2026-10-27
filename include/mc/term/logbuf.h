#pragma once

#include "mc/text/str.h"

// LogBuffer turns what a program writes to its terminal into lines. Colour
// and style sequences (SGR) stay in the lines; carriage returns overwrite the
// line, as progress bars expect; every other control sequence is dropped.
//
// Lines are numbered from 0 in order of arrival and keep their numbers. The
// oldest go once there are more than max_lines, or once their bytes exceed
// max_bytes.
typedef struct LogBuffer LogBuffer;

LogBuffer *logbuf_create(size_t max_lines, size_t max_bytes);
void logbuf_destroy(LogBuffer *log);

void logbuf_write(LogBuffer *log, String output);
// logbuf_note ends an unfinished line and adds text as a line of its own.
void logbuf_note(LogBuffer *log, String text);
void logbuf_clear(LogBuffer *log);

// logbuf_first is the number of the oldest line kept, logbuf_end one past the newest.
uint64_t logbuf_first(const LogBuffer *log);
uint64_t logbuf_end(const LogBuffer *log);
// logbuf_line is a view of line number, which must be kept; it stays valid
// until the next write, note or clear.
String logbuf_line(const LogBuffer *log, uint64_t number);
// logbuf_partial is the line still being written, such as a prompt.
String logbuf_partial(const LogBuffer *log);
