#include "test.h"

#include "mc/platform/platform.h"
#include "mc/platform/service.h"
#include "mc/text/path.h"

// FakeManager records the commands it is given and answers like the service managers would.
typedef struct {
    Arena *arena;
    StringList commands;
    bool loaded;
} FakeManager;

[[nodiscard]] static Error run_fake(void *context, Arena *scratch, StringList argv, Err *err)
{
    FakeManager *fake = context;
    String command = str_join(fake->arena, argv, S(" "));
    strlist_push(fake->arena, &fake->commands, command);
    String verb = argv.items[1];
    if (str_equal(verb, S("bootout")) && !fake->loaded) {
        return err_set(err, ERR_PLATFORM, "%s: Boot-out failed: 3: No such process", str_cstr(scratch, command));
    }
    if (str_equal(verb, S("print")) && !fake->loaded) {
        return err_set(err, ERR_PLATFORM, "%s: Could not find service", str_cstr(scratch, command));
    }
    fake->loaded = !str_equal(verb, S("bootout"));
    return ERR_OK;
}

static Service sample_service(Arena *arena)
{
    const char *const argv[] = { "/opt/my app/sync&co", "run", "--label=50%$HOME\"" };
    return (Service){
        .name = S("sync"),
        .description = S("folder sync"),
        .argv = strlist_of(arena, countof(argv), argv),
        .log_path = S("/Users/me/Library/Logs/sync.log"),
    };
}

static void service_writes_definitions(Test *test)
{
    Arena *a = test->arena;
    Service service = sample_service(a);
    ServiceManager launchd = { .kind = SERVICE_LAUNCHD, .directory = S("/Users/me/Library/LaunchAgents") };
    test_check_str(test, service_path(a, &launchd, S("sync")), S("/Users/me/Library/LaunchAgents/sync.plist"),
                   "the agent's path");
    test_check_str(test, service_definition(a, &launchd, &service),
                   S("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                     "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                     "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                     "<plist version=\"1.0\"><dict>\n"
                     "  <key>Label</key><string>sync</string>\n"
                     "  <key>ProgramArguments</key><array><string>/opt/my app/sync&amp;co</string><string>run</string>"
                     "<string>--label=50%$HOME&#34;</string></array>\n"
                     "  <key>RunAtLoad</key><true/>\n"
                     "  <key>KeepAlive</key><true/>\n"
                     "  <key>StandardOutPath</key><string>/Users/me/Library/Logs/sync.log</string>\n"
                     "  <key>StandardErrorPath</key><string>/Users/me/Library/Logs/sync.log</string>\n"
                     "</dict></plist>\n"),
                   "a launchd property list");
    ServiceManager systemd = { .kind = SERVICE_SYSTEMD, .directory = S("/home/me/.config/systemd/user") };
    test_check_str(test, service_path(a, &systemd, S("sync")), S("/home/me/.config/systemd/user/sync.service"),
                   "the unit's path");
    test_check_str(test, service_definition(a, &systemd, &service),
                   S("[Unit]\n"
                     "Description=folder sync\n"
                     "\n"
                     "[Service]\n"
                     "ExecStart=\"/opt/my app/sync&co\" \"run\" \"--label=50%%$$HOME\\\"\"\n"
                     "Restart=on-failure\n"
                     "RestartSec=5\n"
                     "\n"
                     "[Install]\n"
                     "WantedBy=default.target\n"),
                   "a systemd unit");
}

static void service_enables_and_disables(Test *test)
{
    Arena *a = test->arena;
    String dir;
    if (dir_create_temp(a, S("mc-service-"), &dir, nullptr) != ERR_OK) {
        test_check(test, false, "create a temp dir");
        return;
    }
    Service service = sample_service(a);
    FakeManager fake = { .arena = a };
    ServiceManager launchd = {
        .kind = SERVICE_LAUNCHD, .directory = path_join(a, dir, S("LaunchAgents")), .uid = 501,
        .run = run_fake, .run_context = &fake,
    };
    Err err = { 0 };
    String plist = service_path(a, &launchd, S("sync"));
    test_check(test, service_enable(a, &launchd, &service, &err) == ERR_OK, "enable an agent");
    test_check(test, service_installed(a, &launchd, S("sync")), "the agent is installed");
    test_check_str(test, str_join(a, fake.commands, S("\n")),
                   str_format(a, "launchctl bootout gui/501/sync\nlaunchctl bootstrap gui/501 %s", str_cstr(a, plist)),
                   "a first enable bootstraps the agent");
    fake.commands = (StringList){ 0 };
    test_check(test, service_enable(a, &launchd, &service, &err) == ERR_OK, "enable it again");
    test_check_str(test, str_join(a, fake.commands, S("\n")),
                   str_format(a, "launchctl bootout gui/501/sync\nlaunchctl print gui/501/sync\n"
                                 "launchctl bootstrap gui/501 %s",
                              str_cstr(a, plist)),
                   "enabling a loaded agent waits for it to stop first");
    fake.commands = (StringList){ 0 };
    test_check(test, service_disable(a, &launchd, S("sync"), &err) == ERR_OK, "disable the agent");
    test_check_str(test, str_join(a, fake.commands, S("\n")), S("launchctl bootout gui/501/sync\nlaunchctl print gui/501/sync"),
                   "disabling boots it out");
    test_check(test, !service_installed(a, &launchd, S("sync")), "the agent is gone");
    test_check(test, service_disable(a, &launchd, S("sync"), &err) == ERR_NOT_FOUND, "nothing left to disable");

    fake.commands = (StringList){ 0 };
    ServiceManager systemd = {
        .kind = SERVICE_SYSTEMD, .directory = path_join(a, dir, S("systemd/user")), .run = run_fake, .run_context = &fake,
    };
    test_check(test, service_enable(a, &systemd, &service, &err) == ERR_OK, "enable a unit");
    test_check(test, service_disable(a, &systemd, S("sync"), &err) == ERR_OK, "disable the unit");
    test_check_str(test, str_join(a, fake.commands, S("\n")),
                   S("systemctl --user daemon-reload\nsystemctl --user enable sync.service\n"
                     "systemctl --user restart sync.service\nsystemctl --user disable --now sync.service\n"
                     "systemctl --user daemon-reload"),
                   "systemctl reloads, enables, restarts and disables");
    test_check(test, !service_installed(a, &systemd, S("sync")), "the unit is gone");
    unused(dir_remove_all(dir, nullptr));
}

static void service_finds_its_executable(Test *test)
{
    Arena *a = test->arena;
    String running = { 0 };
    String path = { 0 };
    test_check(test,
               process_executable_path(a, &running, nullptr) == ERR_OK &&
                   service_executable_path(a, S("mc-no-such-program"), &path, nullptr) == ERR_OK,
               "find the executable");
    test_check_str(test, path, running, "the running executable when the name is not on PATH");
    ServiceManager manager;
    test_check(test, service_manager(a, &manager, nullptr) == ERR_OK && manager.directory.len > 0,
               "this system has a service manager");
}

const TestCase SERVICE_TESTS[] = {
    { "service_writes_definitions", service_writes_definitions },
    { "service_enables_and_disables", service_enables_and_disables },
    { "service_finds_its_executable", service_finds_its_executable },
    { nullptr, nullptr },
};
