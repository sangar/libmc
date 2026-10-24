#include "mc/encoding/markdown.h"

#include <limits.h>
#include <string.h>

#include "entity.h"
#include "mc/text/utf8.h"
#include "md4c.h"

enum {
    REPLACEMENT_CHARACTER = 0xFFFD,
    GITHUB_EXTENSIONS = MD_FLAG_TABLES | MD_FLAG_TASKLISTS | MD_FLAG_STRIKETHROUGH | MD_FLAG_PERMISSIVEAUTOLINKS,
};

static const char UTF8_BYTE_ORDER_MARK[] = "\xEF\xBB\xBF";

// The URLs cmark leaves out of links and images unless told otherwise: they
// run code or read local files when opened. Data URLs of common image types are safe.
static const char *const DANGEROUS_SCHEMES[] = { "javascript:", "vbscript:", "file:", "data:" };
static const char *const SAFE_DATA_IMAGES[] = { "data:image/png", "data:image/gif", "data:image/jpeg", "data:image/webp" };

// Renderer writes HTML for md4c's callbacks, laid out as cmark lays out the
// CommonMark spec's examples.
typedef struct Renderer {
    StringBuilder html;
    // attribute holds a URL, title or language with its entities decoded.
    StringBuilder attribute;
    bool allow_html;
    // image_depth counts the images being rendered: inside one, text goes into its alt attribute.
    unsigned image_depth;
} Renderer;

static void append(Renderer *r, const char *text)
{
    str_builder_append(&r->html, S(text));
}

// start_line ends the line before a block's tag, as cmark does.
static void start_line(Renderer *r)
{
    if (r->html.len > 0 && r->html.data[r->html.len - 1] != '\n') {
        str_builder_append_char(&r->html, '\n');
    }
}

static const char *html_entity(char c)
{
    switch (c) {
    case '&':
        return "&amp;";
    case '<':
        return "&lt;";
    case '>':
        return "&gt;";
    case '"':
        return "&quot;";
    default:
        return nullptr;
    }
}

static void append_escaped(StringBuilder *out, String text)
{
    size_t start = 0;
    for (size_t i = 0; i < text.len; i++) {
        const char *entity = html_entity(text.data[i]);
        if (entity != nullptr) {
            str_builder_append(out, str_slice(text, start, i));
            str_builder_append(out, S(entity));
            start = i + 1;
        }
    }
    str_builder_append(out, str_slice(text, start, text.len));
}

static bool is_url_safe(unsigned char c)
{
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           (c != 0 && strchr("-_.+!*(),%#@?=;:/$~", c) != nullptr);
}

// append_url percent-encodes url as cmark does: the characters URLs allow
// stay, and & and ' become entities.
static void append_url(StringBuilder *out, String url)
{
    static const char HEX[] = "0123456789ABCDEF";
    for (size_t i = 0; i < url.len; i++) {
        unsigned char c = (unsigned char)url.data[i];
        if (c == '&') {
            str_builder_append(out, S("&amp;"));
        } else if (c == '\'') {
            str_builder_append(out, S("&#x27;"));
        } else if (is_url_safe(c)) {
            str_builder_append_char(out, (char)c);
        } else {
            str_builder_append_format(out, "%%%c%c", HEX[c >> 4], HEX[c & 0xF]);
        }
    }
}

// append_codepoint writes U+0000, which HTML forbids, as U+FFFD.
static void append_codepoint(StringBuilder *out, uint32_t codepoint)
{
    char bytes[UTF8_MAX_BYTES];
    size_t len = utf8_encode(codepoint == 0 ? REPLACEMENT_CHARACTER : codepoint, bytes);
    str_builder_append(out, (String){ bytes, len });
}

static uint32_t hex_digit(char c)
{
    if ('0' <= c && c <= '9') {
        return (uint32_t)(c - '0');
    }
    return (uint32_t)((c | 0x20) - 'a' + 10);
}

// append_entity writes the character an entity such as &amp;, &#35; or
// &#x22; stands for, or the entity itself when it names none. md4c passes
// only well-formed entities, with at most seven digits.
static void append_entity(StringBuilder *out, String entity)
{
    if (entity.len > 3 && entity.data[1] == '#') {
        bool hex = entity.data[2] == 'x' || entity.data[2] == 'X';
        uint32_t codepoint = 0;
        for (size_t i = hex ? 3 : 2; i + 1 < entity.len; i++) {
            codepoint = codepoint * (hex ? 16 : 10) + hex_digit(entity.data[i]);
        }
        append_codepoint(out, codepoint);
        return;
    }
    const ENTITY *named = entity_lookup(entity.data, entity.len);
    if (named == nullptr) {
        str_builder_append(out, entity);
        return;
    }
    append_codepoint(out, named->codepoints[0]);
    if (named->codepoints[1] != 0) {
        append_codepoint(out, named->codepoints[1]);
    }
}

// attribute_text decodes an attribute, whose entities and NUL characters md4c passes as parts of their own.
static String attribute_text(Renderer *r, const MD_ATTRIBUTE *attribute)
{
    r->attribute.len = 0;
    if (attribute->text == nullptr) {
        return (String){ 0 };
    }
    for (size_t i = 0; attribute->substr_offsets[i] < attribute->size; i++) {
        MD_OFFSET start = attribute->substr_offsets[i];
        String part = { attribute->text + start, attribute->substr_offsets[i + 1] - start };
        switch (attribute->substr_types[i]) {
        case MD_TEXT_ENTITY:
            append_entity(&r->attribute, part);
            break;
        case MD_TEXT_NULLCHAR:
            append_codepoint(&r->attribute, REPLACEMENT_CHARACTER);
            break;
        default:
            str_builder_append(&r->attribute, part);
            break;
        }
    }
    return (String){ r->attribute.data, r->attribute.len };
}

static bool starts_with_ignore_case(String s, const char *prefix)
{
    String p = S(prefix);
    return str_equal_ignore_case(str_slice(s, 0, p.len), p);
}

static bool is_dangerous_url(String url)
{
    for (size_t i = 0; i < countof(SAFE_DATA_IMAGES); i++) {
        if (starts_with_ignore_case(url, SAFE_DATA_IMAGES[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < countof(DANGEROUS_SCHEMES); i++) {
        if (starts_with_ignore_case(url, DANGEROUS_SCHEMES[i])) {
            return true;
        }
    }
    return false;
}

// append_link_url writes url, or nothing when it is dangerous and HTML is not allowed.
static void append_link_url(Renderer *r, const MD_ATTRIBUTE *url)
{
    String decoded = attribute_text(r, url);
    if (r->allow_html || !is_dangerous_url(decoded)) {
        append_url(&r->html, decoded);
    }
}

static void append_title(Renderer *r, const MD_ATTRIBUTE *title)
{
    String decoded = attribute_text(r, title);
    if (decoded.len > 0) {
        append(r, "\" title=\"");
        append_escaped(&r->html, decoded);
    }
}

static void open_code_block(Renderer *r, const MD_BLOCK_CODE_DETAIL *code)
{
    append(r, "<pre><code");
    String language = attribute_text(r, &code->lang);
    if (language.len > 0) {
        append(r, " class=\"language-");
        append_escaped(&r->html, language);
        append(r, "\"");
    }
    append(r, ">");
}

// open_list_item writes a task's checkbox as GitHub's spec does.
static void open_list_item(Renderer *r, const MD_BLOCK_LI_DETAIL *item)
{
    append(r, "<li>");
    if (item->is_task) {
        append(r, item->task_mark == ' ' ? "<input disabled=\"\" type=\"checkbox\"> "
                                         : "<input checked=\"\" disabled=\"\" type=\"checkbox\"> ");
    }
}

static void open_cell(Renderer *r, const char *tag, const MD_BLOCK_TD_DETAIL *cell)
{
    static const char *const ALIGNMENTS[] = {
        [MD_ALIGN_DEFAULT] = "", [MD_ALIGN_LEFT] = " align=\"left\"",
        [MD_ALIGN_CENTER] = " align=\"center\"", [MD_ALIGN_RIGHT] = " align=\"right\"",
    };
    str_builder_append_format(&r->html, "<%s%s>", tag, ALIGNMENTS[cell->align]);
}

static int enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    Renderer *r = userdata;
    switch (type) {
    case MD_BLOCK_QUOTE:
        start_line(r);
        append(r, "<blockquote>\n");
        break;
    case MD_BLOCK_UL:
        start_line(r);
        append(r, "<ul>\n");
        break;
    case MD_BLOCK_OL: {
        start_line(r);
        unsigned start = ((const MD_BLOCK_OL_DETAIL *)detail)->start;
        if (start == 1) {
            append(r, "<ol>\n");
        } else {
            str_builder_append_format(&r->html, "<ol start=\"%u\">\n", start);
        }
        break;
    }
    case MD_BLOCK_LI:
        start_line(r);
        open_list_item(r, detail);
        break;
    case MD_BLOCK_HR:
        start_line(r);
        append(r, "<hr />\n");
        break;
    case MD_BLOCK_H:
        start_line(r);
        str_builder_append_format(&r->html, "<h%u>", ((const MD_BLOCK_H_DETAIL *)detail)->level);
        break;
    case MD_BLOCK_CODE:
        start_line(r);
        open_code_block(r, detail);
        break;
    case MD_BLOCK_HTML:
        start_line(r);
        break;
    case MD_BLOCK_P:
        start_line(r);
        append(r, "<p>");
        break;
    case MD_BLOCK_TABLE:
        start_line(r);
        append(r, "<table>\n");
        break;
    case MD_BLOCK_THEAD:
        append(r, "<thead>\n");
        break;
    case MD_BLOCK_TBODY:
        append(r, "<tbody>\n");
        break;
    case MD_BLOCK_TR:
        append(r, "<tr>\n");
        break;
    case MD_BLOCK_TH:
        open_cell(r, "th", detail);
        break;
    case MD_BLOCK_TD:
        open_cell(r, "td", detail);
        break;
    default:
        // The document itself, and blocks of extensions that are off.
        break;
    }
    return 0;
}

static int leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    Renderer *r = userdata;
    switch (type) {
    case MD_BLOCK_QUOTE:
        start_line(r);
        append(r, "</blockquote>\n");
        break;
    case MD_BLOCK_UL:
        start_line(r);
        append(r, "</ul>\n");
        break;
    case MD_BLOCK_OL:
        start_line(r);
        append(r, "</ol>\n");
        break;
    case MD_BLOCK_LI:
        append(r, "</li>\n");
        break;
    case MD_BLOCK_H:
        str_builder_append_format(&r->html, "</h%u>\n", ((const MD_BLOCK_H_DETAIL *)detail)->level);
        break;
    case MD_BLOCK_CODE:
        append(r, "</code></pre>\n");
        break;
    case MD_BLOCK_HTML:
        start_line(r);
        break;
    case MD_BLOCK_P:
        append(r, "</p>\n");
        break;
    case MD_BLOCK_TABLE:
        append(r, "</table>\n");
        break;
    case MD_BLOCK_THEAD:
        append(r, "</thead>\n");
        break;
    case MD_BLOCK_TBODY:
        append(r, "</tbody>\n");
        break;
    case MD_BLOCK_TR:
        append(r, "</tr>\n");
        break;
    case MD_BLOCK_TH:
        append(r, "</th>\n");
        break;
    case MD_BLOCK_TD:
        append(r, "</td>\n");
        break;
    default:
        break;
    }
    return 0;
}

static int enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    Renderer *r = userdata;
    if (r->image_depth > 0) {
        if (type == MD_SPAN_IMG) {
            r->image_depth++;
        }
        return 0;
    }
    switch (type) {
    case MD_SPAN_EM:
        append(r, "<em>");
        break;
    case MD_SPAN_STRONG:
        append(r, "<strong>");
        break;
    case MD_SPAN_CODE:
        append(r, "<code>");
        break;
    case MD_SPAN_DEL:
        append(r, "<del>");
        break;
    case MD_SPAN_A: {
        const MD_SPAN_A_DETAIL *link = detail;
        append(r, "<a href=\"");
        append_link_url(r, &link->href);
        append_title(r, &link->title);
        append(r, "\">");
        break;
    }
    case MD_SPAN_IMG:
        r->image_depth = 1;
        append(r, "<img src=\"");
        append_link_url(r, &((const MD_SPAN_IMG_DETAIL *)detail)->src);
        append(r, "\" alt=\"");
        break;
    default:
        break;
    }
    return 0;
}

static int leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    Renderer *r = userdata;
    if (r->image_depth > 0) {
        if (type == MD_SPAN_IMG) {
            r->image_depth--;
        }
        if (r->image_depth == 0) {
            append_title(r, &((const MD_SPAN_IMG_DETAIL *)detail)->title);
            append(r, "\" />");
        }
        return 0;
    }
    switch (type) {
    case MD_SPAN_EM:
        append(r, "</em>");
        break;
    case MD_SPAN_STRONG:
        append(r, "</strong>");
        break;
    case MD_SPAN_CODE:
        append(r, "</code>");
        break;
    case MD_SPAN_DEL:
        append(r, "</del>");
        break;
    case MD_SPAN_A:
        append(r, "</a>");
        break;
    default:
        break;
    }
    return 0;
}

static int text(MD_TEXTTYPE type, const MD_CHAR *data, MD_SIZE size, void *userdata)
{
    Renderer *r = userdata;
    String s = { data, size };
    switch (type) {
    case MD_TEXT_NULLCHAR:
        append_codepoint(&r->html, REPLACEMENT_CHARACTER);
        break;
    case MD_TEXT_BR:
        append(r, r->image_depth > 0 ? " " : "<br />\n");
        break;
    case MD_TEXT_SOFTBR:
        append(r, r->image_depth > 0 ? " " : "\n");
        break;
    case MD_TEXT_HTML:
        if (r->image_depth > 0) {
            append_escaped(&r->html, s);
        } else {
            str_builder_append(&r->html, s);
        }
        break;
    case MD_TEXT_ENTITY:
        r->attribute.len = 0;
        append_entity(&r->attribute, s);
        append_escaped(&r->html, (String){ r->attribute.data, r->attribute.len });
        break;
    default:
        append_escaped(&r->html, s);
        break;
    }
    return 0;
}

Error markdown_to_html(Arena *arena, String markdown, MarkdownOptions options, String *html, Err *err)
{
    markdown = str_trim_prefix(markdown, S(UTF8_BYTE_ORDER_MARK));
    if (markdown.len > UINT_MAX) {
        return err_set(err, ERR_INVALID_ARGUMENT, "markdown: %zu bytes is more than md4c reads", markdown.len);
    }
    Renderer r = {
        .html = str_builder_create(arena, markdown.len + markdown.len / 4),
        .attribute = str_builder_create(arena, 64),
        .allow_html = options.allow_html,
    };
    const MD_PARSER parser = {
        .flags = GITHUB_EXTENSIONS | (options.allow_html ? 0 : MD_FLAG_NOHTML),
        .enter_block = enter_block,
        .leave_block = leave_block,
        .enter_span = enter_span,
        .leave_span = leave_span,
        .text = text,
    };
    if (md_parse(markdown.data, (MD_SIZE)markdown.len, &parser, &r) != 0) {
        return err_set(err, ERR_OUT_OF_MEMORY, "markdown: md4c ran out of memory");
    }
    *html = str_builder_finish(&r.html);
    return ERR_OK;
}
