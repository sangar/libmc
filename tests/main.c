#include "test.h"

#include "mc/platform/platform.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void test_check(Test *test, bool ok, const char *what)
{
    test->checks++;
    if (!ok) {
        test->failures++;
        printf("  FAIL %s: %s\n", test->name, what);
    }
}

void test_check_str(Test *test, String actual, String expected, const char *what)
{
    test->checks++;
    if (!str_equal(actual, expected)) {
        test->failures++;
        printf("  FAIL %s: %s\n       expected \"%.*s\"\n       actual   \"%.*s\"\n", test->name, what,
               (int)expected.len, expected.data, (int)actual.len, actual.data);
    }
}

static const char ABORT_FLAG[] = "--abort-case";

void test_check_aborts(Test *test, const char *abort_case, const char *what)
{
    Err err = { 0 };
    String self;
    ProcessResult result = { 0 };
    const char *const argv[] = { "", ABORT_FLAG, abort_case };
    StringList args = strlist_of(test->arena, countof(argv), argv);
    bool ran = process_executable_path(test->arena, &self, &err) == ERR_OK;
    args.items[0] = self;
    ran = ran && process_run(test->arena, args, S(""), &result, &err) == ERR_OK;
    test_check(test, ran && result.signal == SIGABRT, what);
}

// run_abort_case returns only if the case failed to abort.
static int run_abort_case(const char *name)
{
    for (const AbortCase *c = ABORT_CASES; c->name != nullptr; c++) {
        if (strcmp(c->name, name) == 0) {
            c->run();
            return 0;
        }
    }
    fprintf(stderr, "unknown abort case %s\n", name);
    return 2;
}

static unsigned run_table(const TestCase *cases, unsigned *checks)
{
    unsigned failed = 0;
    for (const TestCase *c = cases; c->name != nullptr; c++) {
        Test test = { .name = c->name, .arena = arena_create(1 << 16) };
        c->run(&test);
        arena_destroy(test.arena);
        *checks += test.checks;
        failed += test.failures > 0;
    }
    return failed;
}

enum { SUITE_TIMEOUT_MS = 60 * 1000 };

// A hang, such as a pipe deadlock, fails the suite instead of stalling it.
static void *fail_after_timeout(void *argument)
{
    clock_sleep_ms(SUITE_TIMEOUT_MS);
    fprintf(stderr, "test suite timed out after %d ms\n", SUITE_TIMEOUT_MS);
    abort();
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], ABORT_FLAG) == 0) {
        return run_abort_case(argv[2]);
    }
    Thread watchdog;
    if (thread_start(&watchdog, fail_after_timeout, nullptr, nullptr) == ERR_OK) {
        thread_detach(&watchdog);
    }
    const TestCase *tables[] = { ARENA_TESTS, STR_TESTS, STRMAP_TESTS, IDTABLE_TESTS, TEXT_TESTS, PLATFORM_TESTS, CONCURRENCY_TESTS,
                                   ENCODING_TESTS, LOG_TESTS, WATCH_TESTS, SERVICE_TESTS };
    unsigned failed = 0;
    unsigned checks = 0;
    for (size_t i = 0; i < countof(tables); i++) {
        failed += run_table(tables[i], &checks);
    }
    if (failed > 0) {
        printf("%u test(s) failed, %u checks\n", failed, checks);
        return 1;
    }
    printf("all tests passed, %u checks\n", checks);
    return 0;
}
