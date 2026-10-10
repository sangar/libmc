#include "test.h"

#include "mc/platform/platform.h"
#include "mc/platform/watch.h"
#include "mc/text/path.h"

static bool skip_named_skipped(void *context, String directory)
{
    return str_equal(path_base(directory), S("skipped"));
}

// Seen gathers events until the one a step waits for arrives.
typedef struct {
    Arena *arena;
    WatchEventList events;
} Seen;

static const WatchEvent *find_event(const Seen *seen, String path)
{
    for (size_t i = 0; i < seen->events.count; i++) {
        if (str_equal(seen->events.items[i].path, path)) {
            return &seen->events.items[i];
        }
    }
    return nullptr;
}

static bool has_event(const Seen *seen, String path)
{
    return find_event(seen, path) != nullptr;
}

static bool has_rescan(const Seen *seen, String path)
{
    const WatchEvent *event = find_event(seen, path);
    return event != nullptr && event->rescan;
}

static bool await_event(Watch *watch, Seen *seen, String path)
{
    int64_t deadline = clock_monotonic_ns() + 5LL * NS_PER_SECOND;
    while (!has_event(seen, path) && clock_monotonic_ns() < deadline) {
        if (watch_read(watch, seen->arena, 100, 50, &seen->events, nullptr) != ERR_OK) {
            return false;
        }
    }
    return has_event(seen, path);
}

static bool any_event_below(const Seen *seen, String directory)
{
    for (size_t i = 0; i < seen->events.count; i++) {
        String below;
        if (path_relative(directory, seen->events.items[i].path, &below)) {
            return true;
        }
    }
    return false;
}

static void watch_reports_changes_below_the_roots(Test *test)
{
    Arena *a = test->arena;
    String root;
    String other;
    if (dir_create_temp(a, S("mc-watch-"), &root, nullptr) != ERR_OK ||
        dir_create_temp(a, S("mc-watch-"), &other, nullptr) != ERR_OK) {
        test_check(test, false, "create temp dirs");
        return;
    }
    const char *const setup[] = { "sub", "skipped", "moving/inner" };
    for (size_t i = 0; i < countof(setup); i++) {
        unused(dir_create_all(path_join(a, root, S(setup[i])), 0755, nullptr));
    }
    StringList roots = { 0 };
    strlist_push(a, &roots, root);
    strlist_push(a, &roots, other);
    Watch *watch;
    Err err = { 0 };
    if (watch_create(roots, skip_named_skipped, nullptr, &watch, &err) != ERR_OK) {
        test_check(test, false, err.msg);
        return;
    }
    Seen seen = { .arena = a };

    String file = path_join(a, root, S("sub/a.txt"));
    unused(file_write_all(file, S("a"), 0644, nullptr));
    test_check(test, await_event(watch, &seen, file), "a new file, spelled from the root as given");

    unused(file_write_all(path_join(a, root, S("skipped/x")), S("x"), 0644, nullptr));
    String after = path_join(a, root, S("after"));
    unused(file_write_all(after, S("y"), 0644, nullptr));
    test_check(test, await_event(watch, &seen, after), "a file written after one in a skipped directory");
    test_check(test, !any_event_below(&seen, path_join(a, root, S("skipped"))), "nothing below a skipped directory");

    String fresh = path_join(a, root, S("new"));
    String inside = path_join(a, fresh, S("b.txt"));
    unused(dir_create(fresh, 0755, nullptr));
    unused(file_write_all(inside, S("b"), 0644, nullptr));
    test_check(test, await_event(watch, &seen, inside) || has_rescan(&seen, fresh),
               "a file in a new directory, or the directory to rescan");

    String moved = path_join(a, root, S("moved"));
    unused(file_rename(path_join(a, root, S("moving")), moved, nullptr));
    String moved_file = path_join(a, moved, S("inner/c.txt"));
    unused(file_write_all(moved_file, S("c"), 0644, nullptr));
    test_check(test, await_event(watch, &seen, moved_file), "a file below a moved directory has its new path");
    test_check(test, has_rescan(&seen, moved), "a moved directory is to be rescanned");

    seen.events = (WatchEventList){ 0 };
    unused(file_remove(file, nullptr));
    test_check(test, await_event(watch, &seen, file), "a removed file");
    String elsewhere = path_join(a, other, S("z"));
    unused(file_write_all(elsewhere, S("z"), 0644, nullptr));
    test_check(test, await_event(watch, &seen, elsewhere), "a change below the second root");

    watch_destroy(watch);
    unused(dir_remove_all(root, nullptr));
    unused(dir_remove_all(other, nullptr));
}

static void watch_outlives_many_removed_directories(Test *test)
{
    Arena *a = test->arena;
    String root;
    if (dir_create_temp(a, S("mc-watch-"), &root, nullptr) != ERR_OK) {
        test_check(test, false, "create a temp dir");
        return;
    }
    String many = path_join(a, root, S("many"));
    for (int i = 0; i < 1500; i++) {
        unused(dir_create_all(path_join(a, many, str_format(a, "%d", i)), 0755, nullptr));
    }
    StringList roots = { 0 };
    strlist_push(a, &roots, root);
    Watch *watch;
    Err err = { 0 };
    if (watch_create(roots, nullptr, nullptr, &watch, &err) != ERR_OK) {
        test_check(test, false, err.msg);
        return;
    }
    unused(dir_remove_all(many, nullptr));
    Seen seen = { .arena = a };
    test_check(test, await_event(watch, &seen, many), "the removed directory");
    String later = path_join(a, root, S("later"));
    unused(dir_create(later, 0755, nullptr));
    test_check(test, await_event(watch, &seen, later), "a directory made after the removals");
    String file = path_join(a, later, S("f"));
    unused(file_write_all(file, S("f"), 0644, nullptr));
    test_check(test, await_event(watch, &seen, file), "a file in a directory made after the removals");
    watch_destroy(watch);
    unused(dir_remove_all(root, nullptr));
}

static void watch_rejects_roots_that_are_not_directories(Test *test)
{
    Arena *a = test->arena;
    String dir;
    if (dir_create_temp(a, S("mc-watch-"), &dir, nullptr) != ERR_OK) {
        test_check(test, false, "create a temp dir");
        return;
    }
    String file = path_join(a, dir, S("file"));
    unused(file_write_all(file, S(""), 0644, nullptr));
    Watch *watch;
    StringList missing = { 0 };
    strlist_push(a, &missing, path_join(a, dir, S("missing")));
    StringList not_dir = { 0 };
    strlist_push(a, &not_dir, file);
    test_check(test, watch_create(missing, nullptr, nullptr, &watch, nullptr) == ERR_NOT_FOUND, "a missing root");
    test_check(test, watch_create(not_dir, nullptr, nullptr, &watch, nullptr) == ERR_INVALID_ARGUMENT, "a file as root");
    unused(dir_remove_all(dir, nullptr));
}

const TestCase WATCH_TESTS[] = {
    { "watch_reports_changes_below_the_roots", watch_reports_changes_below_the_roots },
    { "watch_outlives_many_removed_directories", watch_outlives_many_removed_directories },
    { "watch_rejects_roots_that_are_not_directories", watch_rejects_roots_that_are_not_directories },
    { nullptr, nullptr },
};
