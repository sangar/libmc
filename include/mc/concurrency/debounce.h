#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// Debouncer runs a callback once per key after the key has been quiet for a
// delay: every trigger of a key moves its deadline to the delay from now.
// The callback runs on the debouncer's own thread, one key at a time. The
// waiting keys live in an arena that is reset whenever none is left.
typedef struct Debouncer Debouncer;

// DebounceFunction receives a key that stays valid only during the call.
typedef void (*DebounceFunction)(void *context, String key);

// debounce_create starts the debouncer's thread.
[[nodiscard]] Error debounce_create(int64_t delay_ns, DebounceFunction fire, void *context, Debouncer **debouncer,
                                    Err *err);
// debounce_destroy stops the thread after a callback in progress returns;
// keys still waiting are dropped without firing.
void debounce_destroy(Debouncer *debouncer);

void debounce_trigger(Debouncer *debouncer, String key);
// debounce_pending counts the keys waiting to fire.
size_t debounce_pending(Debouncer *debouncer);
