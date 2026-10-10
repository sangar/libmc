#include "mc/text/glob.h"

static bool is_char(String s, size_t i, char c)
{
    return i < s.len && s.data[i] == c;
}

static char escaped_char(String pattern, size_t *p)
{
    if (is_char(pattern, *p, '\\') && *p + 1 < pattern.len) {
        (*p)++;
    }
    return pattern.data[(*p)++];
}

// A ClassItem is one member of a [...] class: a character or a low-high range.
typedef struct {
    unsigned char low;
    unsigned char high;
} ClassItem;

// class_body is where the members of the class opening at p begin, past any negation.
static size_t class_body(String pattern, size_t p, bool *negated)
{
    size_t body = p + 1;
    *negated = is_char(pattern, body, '!') || is_char(pattern, body, '^');
    return *negated ? body + 1 : body;
}

// class_next reads the member at *at and reports false at the closing bracket
// or the end of the pattern. A ] first in the body is a member, and a
// backslash escapes the character after it, ] included.
static bool class_next(String pattern, size_t *at, size_t body, ClassItem *item)
{
    if (*at >= pattern.len || (pattern.data[*at] == ']' && *at > body)) {
        return false;
    }
    item->low = (unsigned char)escaped_char(pattern, at);
    item->high = item->low;
    if (is_char(pattern, *at, '-') && *at + 1 < pattern.len && pattern.data[*at + 1] != ']') {
        (*at)++;
        item->high = (unsigned char)escaped_char(pattern, at);
    }
    return true;
}

// class_end stores the position past the class opening at p, or reports false when it never closes.
static bool class_end(String pattern, size_t p, size_t *end)
{
    bool negated;
    size_t body = class_body(pattern, p, &negated);
    size_t at = body;
    ClassItem item;
    while (class_next(pattern, &at, body, &item)) {
    }
    if (at == pattern.len) {
        return false;
    }
    *end = at + 1;
    return true;
}

static bool class_matches(String pattern, size_t p, char c)
{
    bool negated;
    size_t body = class_body(pattern, p, &negated);
    size_t at = body;
    ClassItem item;
    bool matched = false;
    while (class_next(pattern, &at, body, &item)) {
        matched = matched || ((unsigned char)c >= item.low && (unsigned char)c <= item.high);
    }
    return matched != negated;
}

static bool is_any_depth(String pattern, size_t p)
{
    bool element_start = p == 0 || pattern.data[p - 1] == '/';
    bool element_end = p + 2 == pattern.len || is_char(pattern, p + 2, '/');
    return element_start && is_char(pattern, p, '*') && is_char(pattern, p + 1, '*') && element_end;
}

// matches_one tries the pattern item at *p against path[i]: on a match it
// moves *p past the item. * and ** are handled by glob_match.
static bool matches_one(String pattern, size_t *p, String path, size_t i)
{
    if (i == path.len) {
        return false;
    }
    size_t at = *p;
    bool matched;
    switch (pattern.data[at]) {
    case '?':
        at++;
        matched = path.data[i] != '/';
        break;
    case '[': {
        size_t end;
        if (!class_end(pattern, at, &end)) {
            at++;
            matched = path.data[i] == '[';
            break;
        }
        matched = path.data[i] != '/' && class_matches(pattern, at, path.data[i]);
        at = end;
        break;
    }
    default:
        matched = escaped_char(pattern, &at) == path.data[i];
    }
    if (matched) {
        *p = at;
    }
    return matched;
}

// Restart is where to resume after a mismatch: the pattern just past a * or
// **, and the next path position for it to try.
typedef struct {
    bool set;
    size_t p;
    size_t i;
} Restart;

// glob_match only ever resumes from the latest * and the latest **: a later
// wildcard can absorb whatever an earlier one would have, so matching takes
// time proportional to the pattern length times the path length.
bool glob_match(String pattern, String path)
{
    size_t p = 0;
    size_t i = 0;
    Restart star = { 0 };
    Restart any_depth = { 0 };
    while (p < pattern.len || i < path.len) {
        if (p < pattern.len) {
            if (is_any_depth(pattern, p)) {
                if (p + 2 == pattern.len) {
                    return true;
                }
                p += 3;
                any_depth = (Restart){ .set = true, .p = p, .i = i };
                star = (Restart){ 0 };
                continue;
            }
            if (pattern.data[p] == '*') {
                while (is_char(pattern, p, '*')) {
                    p++;
                }
                star = (Restart){ .set = true, .p = p, .i = i };
                continue;
            }
            if (matches_one(pattern, &p, path, i)) {
                i++;
                continue;
            }
        }
        if (star.set && star.i < path.len && path.data[star.i] != '/') {
            star.i++;
            p = star.p;
            i = star.i;
            continue;
        }
        size_t slash;
        if (any_depth.set && str_find_char(str_slice(path, any_depth.i, path.len), '/', &slash)) {
            any_depth.i += slash + 1;
            p = any_depth.p;
            i = any_depth.i;
            star = (Restart){ 0 };
            continue;
        }
        return false;
    }
    return true;
}

bool glob_valid(String pattern)
{
    for (size_t p = 0; p < pattern.len; p++) {
        if (pattern.data[p] == '\\') {
            if (++p == pattern.len) {
                return false;
            }
        } else if (pattern.data[p] == '[') {
            size_t end;
            if (!class_end(pattern, p, &end)) {
                return false;
            }
            p = end - 1;
        }
    }
    return true;
}
