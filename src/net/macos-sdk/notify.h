// The notify(3) calls c-ares's src/lib/event/ares_event_configchg.c makes to
// learn of changes to the DNS configuration, for cross builds without the
// macOS SDK; see SystemConfiguration/SCDynamicStoreCopySpecific.h.
#pragma once

#include <stdint.h>

#define NOTIFY_STATUS_OK 0

uint32_t notify_register_file_descriptor(const char *name, int *notify_fd, int flags, int *out_token);
uint32_t notify_cancel(int token);
