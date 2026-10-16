#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

// dotenv_parse reads the text of a .env file into NAME=value entries in the
// arena, in the order written. A line is NAME=value, optionally after
// "export "; blank lines and lines starting with # are skipped. A value in
// single quotes is taken as written; one in double quotes reads \n, \t, \",
// \\ and \$ as escapes; an unquoted value is trimmed and ends at a # that
// follows a space or tab. Nothing is expanded. A line of any other form is
// ERR_PARSE with its line number in err.
[[nodiscard]] Error dotenv_parse(Arena *arena, String text, StringList *entries, Err *err);
