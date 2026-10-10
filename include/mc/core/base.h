#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define countof(array) (sizeof(array) / sizeof((array)[0]))
#define unused(x) ((void)(x))

enum {
    NS_PER_MICROSECOND = 1000,
    NS_PER_MILLISECOND = 1000 * 1000,
    NS_PER_SECOND = 1000 * 1000 * 1000,
};

static inline size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}

static inline size_t max_size(size_t a, size_t b)
{
    return a > b ? a : b;
}
