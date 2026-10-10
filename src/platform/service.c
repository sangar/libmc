#include "mc/platform/service.h"

#include <unistd.h>

#include "mc/platform/platform.h"
#include "mc/text/path.h"

enum { BOOTOUT_POLLS = 150, BOOTOUT_POLL_MS = 100 };

Error service_manager(Arena *arena, ServiceManager *manager, Err *err)
{
    String home = env_home(arena);
    *manager = (ServiceManager){ .uid = (int)getuid(), .run = service_run_command };
#if defined(__APPLE__)
    manager->kind = SERVICE_LAUNCHD;
    manager->directory = path_join(arena, home, S("Library/LaunchAgents"));
    return ERR_OK;
#elif defined(__linux__)
    String config_home;
    if (!env_get(arena, S("XDG_CONFIG_HOME"), &config_home) || config_home.len == 0) {
        config_home = path_join(arena, home, S(".config"));
    }
    manager->kind = SERVICE_SYSTEMD;
    manager->directory = path_join(arena, config_home, S("systemd/user"));
    return ERR_OK;
#else
    return err_set(err, ERR_UNSUPPORTED, "no user service manager is known for this system");
#endif
}

Error service_run_command(void *context, Arena *scratch, StringList argv, Err *err)
{
    ProcessResult result;
    Error e = process_run(scratch, argv, S(""), &result, err);
    if (e != ERR_OK || result.exit_code == 0) {
        return e;
    }
    String output = str_trim(str_concat(scratch, result.stdout_text, result.stderr_text));
    String command = str_join(scratch, argv, S(" "));
    return err_set(err, ERR_PLATFORM, "%.*s: %.*s", (int)command.len, command.data, (int)output.len, output.data);
}

String service_path(Arena *arena, const ServiceManager *manager, String name)
{
    String file = str_concat(arena, name, manager->kind == SERVICE_LAUNCHD ? S(".plist") : S(".service"));
    return path_join(arena, manager->directory, file);
}

static void append_xml(StringBuilder *builder, String s)
{
    for (size_t i = 0; i < s.len; i++) {
        switch (s.data[i]) {
        case '&': str_builder_append(builder, S("&amp;")); break;
        case '<': str_builder_append(builder, S("&lt;")); break;
        case '>': str_builder_append(builder, S("&gt;")); break;
        case '"': str_builder_append(builder, S("&#34;")); break;
        case '\'': str_builder_append(builder, S("&#39;")); break;
        default: str_builder_append_char(builder, s.data[i]);
        }
    }
}

static void append_xml_string(StringBuilder *builder, String s)
{
    str_builder_append(builder, S("<string>"));
    append_xml(builder, s);
    str_builder_append(builder, S("</string>"));
}

static String launchd_plist(Arena *arena, const Service *service)
{
    StringBuilder builder = str_builder_create(arena, 1024);
    str_builder_append(&builder, S("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                   "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                   "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                   "<plist version=\"1.0\"><dict>\n"
                                   "  <key>Label</key>"));
    append_xml_string(&builder, service->name);
    str_builder_append(&builder, S("\n  <key>ProgramArguments</key><array>"));
    for (size_t i = 0; i < service->argv.count; i++) {
        append_xml_string(&builder, service->argv.items[i]);
    }
    str_builder_append(&builder, S("</array>\n"
                                   "  <key>RunAtLoad</key><true/>\n"
                                   "  <key>KeepAlive</key><true/>\n"));
    if (service->log_path.len > 0) {
        str_builder_append(&builder, S("  <key>StandardOutPath</key>"));
        append_xml_string(&builder, service->log_path);
        str_builder_append(&builder, S("\n  <key>StandardErrorPath</key>"));
        append_xml_string(&builder, service->log_path);
        str_builder_append_char(&builder, '\n');
    }
    str_builder_append(&builder, S("</dict></plist>\n"));
    return str_builder_finish(&builder);
}

// append_exec_argument quotes an ExecStart= argument, doubling the % and $ that systemd would expand.
static void append_exec_argument(StringBuilder *builder, String argument)
{
    str_builder_append_char(builder, '"');
    for (size_t i = 0; i < argument.len; i++) {
        char c = argument.data[i];
        if (c == '"' || c == '\\') {
            str_builder_append_char(builder, '\\');
        } else if (c == '%' || c == '$') {
            str_builder_append_char(builder, c);
        }
        str_builder_append_char(builder, c);
    }
    str_builder_append_char(builder, '"');
}

static String systemd_unit(Arena *arena, const Service *service)
{
    StringBuilder builder = str_builder_create(arena, 512);
    str_builder_append_format(&builder, "[Unit]\nDescription=%.*s\n\n[Service]\nExecStart=", (int)service->description.len,
                              service->description.data);
    for (size_t i = 0; i < service->argv.count; i++) {
        if (i > 0) {
            str_builder_append_char(&builder, ' ');
        }
        append_exec_argument(&builder, service->argv.items[i]);
    }
    str_builder_append(&builder, S("\nRestart=on-failure\n"
                                   "RestartSec=5\n"
                                   "\n"
                                   "[Install]\n"
                                   "WantedBy=default.target\n"));
    return str_builder_finish(&builder);
}

String service_definition(Arena *arena, const ServiceManager *manager, const Service *service)
{
    return manager->kind == SERVICE_LAUNCHD ? launchd_plist(arena, service) : systemd_unit(arena, service);
}

bool service_installed(Arena *scratch, const ServiceManager *manager, String name)
{
    return file_exists(service_path(scratch, manager, name));
}

[[nodiscard]] static Error run(Arena *scratch, const ServiceManager *manager, Err *err, size_t count,
                               const String *argv)
{
    StringList list = { 0 };
    for (size_t i = 0; i < count; i++) {
        strlist_push(scratch, &list, argv[i]);
    }
    return manager->run(manager->run_context, scratch, list, err);
}

// bootout unloads the agent if launchd has it and waits until it is gone:
// launchctl returns as soon as it sent the stop signal, and a bootstrap while
// the old registration lingers fails with an I/O error.
[[nodiscard]] static Error bootout(Arena *scratch, const ServiceManager *manager, String name, Err *err)
{
    String target = str_format(scratch, "gui/%d/%.*s", manager->uid, (int)name.len, name.data);
    Err detail = { 0 };
    Error e = run(scratch, manager, &detail, 3, (const String[]){ S("launchctl"), S("bootout"), target });
    if (e != ERR_OK) {
        bool not_loaded = str_contains(S(detail.msg), S("No such process")) ||
                          str_contains(S(detail.msg), S("Could not find specified service"));
        return not_loaded ? ERR_OK : err_set(err, e, "%s", detail.msg);
    }
    for (int i = 0; i < BOOTOUT_POLLS; i++) {
        if (run(scratch, manager, nullptr, 3, (const String[]){ S("launchctl"), S("print"), target }) != ERR_OK) {
            return ERR_OK;
        }
        clock_sleep_ms(BOOTOUT_POLL_MS);
    }
    return err_set(err, ERR_TIMEOUT, "launchctl bootout: %.*s did not stop within %d seconds", (int)name.len, name.data,
                   BOOTOUT_POLLS * BOOTOUT_POLL_MS / 1000);
}

Error service_enable(Arena *scratch, const ServiceManager *manager, const Service *service, Err *err)
{
    String path = service_path(scratch, manager, service->name);
    Error e = file_write_atomic(path, service_definition(scratch, manager, service), 0644, err);
    if (e != ERR_OK) {
        return e;
    }
    if (manager->kind == SERVICE_LAUNCHD) {
        String domain = str_format(scratch, "gui/%d", manager->uid);
        e = bootout(scratch, manager, service->name, err);
        return e != ERR_OK ? e : run(scratch, manager, err, 4, (const String[]){ S("launchctl"), S("bootstrap"), domain, path });
    }
    String unit = str_concat(scratch, service->name, S(".service"));
    e = run(scratch, manager, err, 3, (const String[]){ S("systemctl"), S("--user"), S("daemon-reload") });
    if (e == ERR_OK) {
        e = run(scratch, manager, err, 4, (const String[]){ S("systemctl"), S("--user"), S("enable"), unit });
    }
    if (e == ERR_OK) {
        e = run(scratch, manager, err, 4, (const String[]){ S("systemctl"), S("--user"), S("restart"), unit });
    }
    return e;
}

Error service_disable(Arena *scratch, const ServiceManager *manager, String name, Err *err)
{
    String path = service_path(scratch, manager, name);
    if (!file_exists(path)) {
        return err_set(err, ERR_NOT_FOUND, "no service installed at %.*s", (int)path.len, path.data);
    }
    String unit = str_concat(scratch, name, S(".service"));
    Error e = manager->kind == SERVICE_LAUNCHD
                  ? bootout(scratch, manager, name, err)
                  : run(scratch, manager, err, 5,
                        (const String[]){ S("systemctl"), S("--user"), S("disable"), S("--now"), unit });
    if (e == ERR_OK) {
        e = file_remove(path, err);
    }
    if (e == ERR_OK && manager->kind == SERVICE_SYSTEMD) {
        e = run(scratch, manager, err, 3, (const String[]){ S("systemctl"), S("--user"), S("daemon-reload") });
    }
    return e;
}

Error service_executable_path(Arena *arena, String name, String *path, Err *err)
{
    String running;
    Error e = process_executable_path(arena, &running, err);
    if (e != ERR_OK) {
        return e;
    }
    String on_path;
    *path = process_look_path(arena, name, &on_path) && paths_are_same_file(on_path, running) ? on_path : running;
    return ERR_OK;
}
