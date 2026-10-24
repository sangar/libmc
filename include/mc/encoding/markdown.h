#pragma once

#include "mc/core/error.h"
#include "mc/text/str.h"

typedef struct MarkdownOptions {
    // allow_html passes raw HTML through and keeps links and images of every
    // scheme. Only for trusted text: it lets the author run scripts in the page.
    bool allow_html;
} MarkdownOptions;

// markdown_to_html renders CommonMark 0.31.2 with GitHub's tables, task
// lists, strikethrough and autolinks as HTML in arena, in the form of the
// CommonMark spec's examples. Raw HTML is shown as text, and a link or image
// to a javascript:, vbscript:, file: or data: URL gets an empty one, except a
// PNG, GIF, JPEG or WebP data: image; options.allow_html turns both off.
// Text of 4 GiB or more is ERR_INVALID_ARGUMENT.
[[nodiscard]] Error markdown_to_html(Arena *arena, String markdown, MarkdownOptions options, String *html, Err *err);
