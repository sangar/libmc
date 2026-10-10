#pragma once

#include "mc/text/str.h"

// glob_match is shell-style matching of a slash-separated path: * and ?
// never cross a slash, [...] is a class with ranges and ! or ^ negation, a
// backslash escapes, and ** as a whole path element matches any number of
// elements.
bool glob_match(String pattern, String path);
// glob_valid rejects an unterminated class or a trailing backslash. Inside a
// class a backslash escapes the next character, so [a\] never closes;
// glob_match reads the [ of a class that never closes as a literal.
bool glob_valid(String pattern);
