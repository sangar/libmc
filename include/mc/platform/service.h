#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// A per-user login service: a launchd agent on macOS, a systemd user unit on
// Linux. Enabling one starts it now, in place of an instance already
// running, and at every login, and restarts it when it fails.

typedef struct Service {
    // name is the launchd label and the systemd unit's name, such as "dbox".
    String name;
    // description becomes systemd's Description=.
    String description;
    // argv is the program, as an absolute path, and its arguments.
    StringList argv;
    // log_path receives the output under launchd; empty discards it. systemd keeps output in its journal.
    String log_path;
} Service;

typedef enum ServiceManagerKind { SERVICE_LAUNCHD, SERVICE_SYSTEMD } ServiceManagerKind;

// ServiceRunner runs one launchctl or systemctl command. It fails when the
// command exits non-zero, with the command and its output in err.
typedef Error (*ServiceRunner)(void *context, Arena *scratch, StringList argv, Err *err);

typedef struct ServiceManager {
    ServiceManagerKind kind;
    // directory holds the definitions: ~/Library/LaunchAgents or ~/.config/systemd/user.
    String directory;
    // uid names launchd's gui/<uid> domain.
    int uid;
    ServiceRunner run;
    void *run_context;
} ServiceManager;

// service_manager describes this user's session, with service_run_command as
// its runner. It is ERR_UNSUPPORTED on systems other than macOS and Linux.
[[nodiscard]] Error service_manager(Arena *arena, ServiceManager *manager, Err *err);
// service_run_command is the ServiceRunner that runs the command with process_run.
[[nodiscard]] Error service_run_command(void *context, Arena *scratch, StringList argv, Err *err);

// service_path is where the definition of the service called name goes.
String service_path(Arena *arena, const ServiceManager *manager, String name);
// service_definition is the launchd property list or systemd unit for service.
String service_definition(Arena *arena, const ServiceManager *manager, const Service *service);
bool service_installed(Arena *scratch, const ServiceManager *manager, String name);

// service_enable writes the definition and starts the service.
[[nodiscard]] Error service_enable(Arena *scratch, const ServiceManager *manager, const Service *service, Err *err);
// service_disable stops the service and removes its definition; ERR_NOT_FOUND when none is installed.
[[nodiscard]] Error service_disable(Arena *scratch, const ServiceManager *manager, String name, Err *err);

// service_executable_path is name found on PATH when that is the running
// binary, a path that survives upgrades moving the real file, and else the
// running executable's resolved path.
[[nodiscard]] Error service_executable_path(Arena *arena, String name, String *path, Err *err);
