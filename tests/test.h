#pragma once

#include "mc/core/arena.h"
#include "mc/text/str.h"

// Test is one case's state: a fresh arena and its tally of checks.
typedef struct {
    const char *name;
    Arena *arena;
    unsigned checks;
    unsigned failures;
} Test;

// test_check records one assertion; what is printed on failure.
void test_check(Test *test, bool ok, const char *what);
void test_check_str(Test *test, String actual, String expected, const char *what);

// test_check_aborts runs the named ABORT_CASES entry in a child test process
// and checks that it ends with SIGABRT.
void test_check_aborts(Test *test, const char *abort_case, const char *what);

typedef void (*TestFunction)(Test *test);

typedef struct {
    const char *name;
    TestFunction run;
} TestCase;

// AbortCase is code that must abort, such as an allocation size that overflows.
typedef struct {
    const char *name;
    void (*run)(void);
} AbortCase;

// Each test file exports one table, ended by a { nullptr, nullptr } entry.
extern const TestCase ARENA_TESTS[];
extern const TestCase STR_TESTS[];
extern const TestCase STRMAP_TESTS[];
extern const TestCase IDTABLE_TESTS[];
extern const TestCase TEXT_TESTS[];
extern const TestCase PLATFORM_TESTS[];
extern const TestCase CONCURRENCY_TESTS[];
extern const TestCase ENCODING_TESTS[];
extern const TestCase LOG_TESTS[];
extern const TestCase WATCH_TESTS[];
extern const TestCase SERVICE_TESTS[];
extern const AbortCase ABORT_CASES[];
