#define _GNU_SOURCE

#include <pwd.h>

#include "mc/platform/procinfo.h"

String user_name(Arena *arena, int user_id)
{
    struct passwd account;
    struct passwd *found = nullptr;
    char buffer[1024];
    if (getpwuid_r((uid_t)user_id, &account, buffer, sizeof buffer, &found) == 0 && found != nullptr) {
        return str_copy(arena, S(found->pw_name));
    }
    return str_format(arena, "%d", user_id);
}
