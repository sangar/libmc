#pragma once

#include "mc/core/base.h"

enum { HISTORY_CAPACITY = 600 };

// History keeps the newest HISTORY_CAPACITY samples of one series.
// History history = { 0 }; is an empty history.
typedef struct History {
    float values[HISTORY_CAPACITY];
    size_t count;
    size_t next;
} History;

void history_push(History *history, float value);
// history_at is the sample age steps before the newest, 0 when there is none.
float history_at(const History *history, size_t age);
// history_newest is the newest sample, 0 in an empty history.
float history_newest(const History *history);
// history_peak is the largest of the newest span samples, 0 in an empty history.
float history_peak(const History *history, size_t span);
