#include "mc/term/history.h"

void history_push(History *history, float value)
{
    history->values[history->next] = value;
    history->next = (history->next + 1) % HISTORY_CAPACITY;
    history->count = history->count < HISTORY_CAPACITY ? history->count + 1 : HISTORY_CAPACITY;
}

float history_at(const History *history, size_t age)
{
    if (age >= history->count) {
        return 0.0f;
    }
    return history->values[(history->next + HISTORY_CAPACITY - 1 - age) % HISTORY_CAPACITY];
}

float history_newest(const History *history)
{
    return history_at(history, 0);
}

float history_peak(const History *history, size_t span)
{
    float peak = 0.0f;
    for (size_t age = 0; age < span && age < history->count; age++) {
        float value = history_at(history, age);
        peak = value > peak ? value : peak;
    }
    return peak;
}
