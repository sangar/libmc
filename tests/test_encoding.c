#include "test.h"

#include <locale.h>
#include <math.h>

#include "mc/encoding/csv.h"
#include "mc/encoding/dotenv.h"
#include "mc/encoding/json.h"
#include "mc/encoding/markdown.h"
#include "mc/encoding/xml.h"
#include "mc/encoding/yaml.h"
#include "mc/platform/platform.h"

static Node *parse(Test *test, const char *text)
{
    Node *root = nullptr;
    Err err = { 0 };
    if (json_parse(test->arena, S(text), &root, &err) != ERR_OK) {
        test_check(test, false, err.msg);
    }
    return root;
}

static void json_parses_every_kind(Test *test)
{
    Node *root = parse(test, " {\"name\": \"blåbær\", \"id\": 9007199254740993, \"ratio\": -1.5e2,"
                             " \"on\": true, \"off\": false, \"none\": null, \"list\": [1, [], {}]} ");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, node_get_string(root, S("name"), S("")), S("blåbær"), "a string");
    test_check(test, node_get_int(root, S("id"), 0) == 9007199254740993, "an int beyond 2^53 is exact");
    const Node *ratio = node_get(root, S("ratio"));
    test_check(test, ratio->kind == NODE_FLOAT && ratio->number == -150.0, "an exponent makes a float");
    test_check(test, node_get_bool(root, S("on"), false) && !node_get_bool(root, S("off"), true), "booleans");
    test_check(test, node_get(root, S("none"))->kind == NODE_NULL, "null");
    const Node *list = node_get(root, S("list"));
    test_check(test, list->kind == NODE_SEQUENCE && list->count == 3 && list->items[1]->kind == NODE_SEQUENCE &&
                         list->items[2]->kind == NODE_MAPPING,
               "nested containers");
    test_check(test, node_get_int(root, S("name"), 7) == 7 && node_get_string(root, S("gone"), S("x")).len == 1,
               "the getters fall back on a missing key or another kind");
    test_check(test, node_get(list, S("name")) == nullptr, "node_get on a sequence");

    Node *repeated = parse(test, "{\"a\": 1, \"b\": 2, \"a\": 3}");
    test_check(test, repeated != nullptr && repeated->entry_count == 2 && repeated->entries[0].value->integer == 3,
               "a repeated key keeps its first place and its last value");
}

static void json_decodes_strings(Test *test)
{
    Node *root = parse(test, "[\"tab\\tquote\\\"slash\\/\", \"\\u00e6\\ud83d\\ude00\", \"\"]");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, root->items[0]->text, S("tab\tquote\"slash/"), "simple escapes");
    test_check_str(test, root->items[1]->text, S("æ😀"), "\\u escapes and a surrogate pair");
    test_check(test, root->items[2]->text.len == 0, "the empty string");
}

static void json_rejects_malformed_input(Test *test)
{
    const char *const invalid[] = {
        "",          "[1,]",      "{\"a\":1,}", "{'a':1}", "01",      "1.",       ".5",    "-",
        "1e",        "\"\\x\"",   "\"open",    "[1 2]",   "{\"a\" 1}", "tru",     "nul",   "\"\x01\"",
        "\"\xff\"",  "1 2",       "1e400",     "[",       "{\"a\":}", "\"\\u12\"", "NaN",     "\"\\ud800x\"",
    };
    for (size_t i = 0; i < countof(invalid); i++) {
        Node *root;
        if (json_parse(test->arena, S(invalid[i]), &root, nullptr) != ERR_PARSE) {
            test_check(test, false, str_cstr(test->arena, str_format(test->arena, "rejects %s", invalid[i])));
        }
    }
    String deep = str_concat(test->arena, str_repeat(test->arena, S("["), 600), str_repeat(test->arena, S("]"), 600));
    Node *root;
    Err err = { 0 };
    test_check(test, json_parse(test->arena, deep, &root, &err) == ERR_PARSE, "deep nesting is refused");
    test_check(test, json_parse(test->arena, S("{\n  \"a\": x\n}"), &root, &err) == ERR_PARSE, "a bad value");
    test_check_str(test, S(err.msg), S("json: line 2, column 8: unexpected character, expected a JSON value"), "errors carry line and column");
}

static void json_encodes_and_quotes(Test *test)
{
    Node *root = parse(test, "{\"a\": [1, -0.1, 1e300, \"x\\ny\"], \"b\": {\"c\": null, \"d\": false}}");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, json_encode(test->arena, root),
                   S("{\"a\":[1,-0.1,1e300,\"x\\ny\"],\"b\":{\"c\":null,\"d\":false}}"), "compact output");
    Node *nan = node_create(test->arena, NODE_FLOAT);
    nan->number = NAN;
    test_check_str(test, json_encode(test->arena, nan), S("null"), "not a number becomes null");
    Node *invalid = node_scalar(test->arena, NODE_STRING, S("a\xff"));
    test_check_str(test, json_encode(test->arena, invalid), S("\"a\xEF\xBF\xBD\""), "invalid UTF-8 is replaced");
    test_check_str(test, json_quote(test->arena, S("a\"\\\x01\xff\xc3\xa6")), S("\"a\\\"\\\\\\u0001\xEF\xBF\xBD\xc3\xa6\""),
                   "control characters are escaped and invalid UTF-8 replaced");
}

static void json_reads_numbers_of_any_length(Test *test)
{
    Arena *a = test->arena;
    String zeros = str_repeat(a, S("0"), 100);
    const String exact[] = {
        str_concat(a, S("1."), zeros),
        str_concat(a, str_concat(a, S("1"), zeros), S("e-100")),
        str_concat(a, str_concat(a, S("0."), zeros), S("1e101")),
    };
    for (size_t i = 0; i < countof(exact); i++) {
        Node *root = nullptr;
        bool parsed = json_parse(a, exact[i], &root, nullptr) == ERR_OK;
        test_check(test, parsed && root->number == 1.0, str_cstr(a, str_format(a, "%.*s is 1", (int)exact[i].len, exact[i].data)));
    }
    const String too_large[] = {
        str_concat(a, S("1"), str_repeat(a, zeros, 4)),
        str_concat(a, str_concat(a, S("0."), zeros), S("1e410")),
    };
    for (size_t i = 0; i < countof(too_large); i++) {
        Node *root;
        test_check(test, json_parse(a, too_large[i], &root, nullptr) == ERR_PARSE, "a long number beyond a double");
    }
}

static void json_keeps_only_the_tree_and_buffer(Test *test)
{
    enum { COUNT = 10000, BLOCK_SIZE = 16 << 20 };
    String text = str_concat(test->arena, S("[0"), str_concat(test->arena, str_repeat(test->arena, S(",0"), COUNT - 1), S("]")));

    Arena *by_hand = arena_create(BLOCK_SIZE);
    Node *array = node_create(by_hand, NODE_SEQUENCE);
    for (size_t i = 0; i < COUNT; i++) {
        node_append(by_hand, array, node_create(by_hand, NODE_INT));
    }
    size_t tree_bytes = arena_bytes_used(by_hand);
    arena_destroy(by_hand);

    Arena *parsed = arena_create(BLOCK_SIZE);
    Node *root = nullptr;
    bool ok = json_parse(parsed, text, &root, nullptr) == ERR_OK;
    size_t parse_bytes = arena_bytes_used(parsed);
    arena_destroy(parsed);

    enum { PADDING_AND_ALIGNMENT = 64 };
    test_check(test, ok && parse_bytes <= tree_bytes + text.len + PADDING_AND_ALIGNMENT,
               "the caller's arena holds the tree and the parse buffer, not the yyjson document");
}

static void json_floats_ignore_the_locale(Test *test)
{
    const char *const comma_locales[] = { "nb_NO.UTF-8", "de_DE.UTF-8", "fr_FR.UTF-8" };
    bool switched = false;
    for (size_t i = 0; i < countof(comma_locales) && !switched; i++) {
        switched = setlocale(LC_NUMERIC, comma_locales[i]) != nullptr;
    }
    Node *root = parse(test, "[1.5]");
    String encoded = root != nullptr ? json_encode(test->arena, root) : S("");
    setlocale(LC_NUMERIC, "C");
    test_check(test, root != nullptr && root->items[0]->number == 1.5, "1.5 parses with a decimal comma locale");
    test_check_str(test, encoded, S("[1.5]"), "and encodes with a point");
}

static Node *parse_yaml(Test *test, const char *text)
{
    Node *root = nullptr;
    Err err = { 0 };
    if (yaml_parse(test->arena, S(text), &root, &err) != ERR_OK) {
        test_check(test, false, err.msg);
    }
    return root;
}

static void yaml_types_scalars_by_the_core_schema(Test *test)
{
    Node *root = parse_yaml(test, "nulls: [~, null, NULL]\n"
                                  "empty:\n"
                                  "bools: [true, False, TRUE]\n"
                                  "ints: [42, -7, 0x1F, 0o17, +3]\n"
                                  "floats: [1.5, -2e3, .inf, -.Inf, 9223372036854775808]\n"
                                  "strings: ['12', \"true\", !!str 7, ! 8, 0x1G, yes, 1.2.3]\n");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, json_encode(test->arena, root),
                   S("{\"nulls\":[null,null,null],\"empty\":null,\"bools\":[true,false,true],\"ints\":[42,-7,31,15,3],"
                     "\"floats\":[1.5,-2000.0,null,null,9223372036854776000.0],"
                     "\"strings\":[\"12\",\"true\",\"7\",\"8\",\"0x1G\",\"yes\",\"1.2.3\"]}"),
                   "each scalar gets its kind");
    const Node *floats = node_get(root, S("floats"));
    test_check(test, floats->items[2]->kind == NODE_FLOAT && floats->items[2]->number > 0 && isinf(floats->items[3]->number) &&
                         floats->items[3]->number < 0,
               "infinities");
    test_check_str(test, node_get(root, S("ints"))->items[2]->text, S("0x1F"), "a number keeps its text");
    Node *nan = parse_yaml(test, ".nan");
    test_check(test, nan != nullptr && nan->kind == NODE_FLOAT && isnan(nan->number), "not a number");
}

static void yaml_reads_block_and_flow_collections(Test *test)
{
    Node *root = parse_yaml(test, "# a config\n"
                                  "base: &base\n"
                                  "  image: postgres:17\n"
                                  "  ports: [5432, {host: 6432}]\n"
                                  "nodes:\n"
                                  "  - name: a\n"
                                  "    spec: *base\n"
                                  "  - name: b\n"
                                  "script: |\n"
                                  "  one\n"
                                  "  two\n"
                                  "folded: >-\n"
                                  "  one\n"
                                  "  two\n"
                                  "quoted: \"tab\\tæ\\u00e6\"\n"
                                  "name: first\n"
                                  "name: last\n");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, json_encode(test->arena, root),
                   S("{\"base\":{\"image\":\"postgres:17\",\"ports\":[5432,{\"host\":6432}]},"
                     "\"nodes\":[{\"name\":\"a\",\"spec\":{\"image\":\"postgres:17\",\"ports\":[5432,{\"host\":6432}]}},"
                     "{\"name\":\"b\"}],\"script\":\"one\\ntwo\\n\",\"folded\":\"one two\",\"quoted\":\"tab\\tææ\","
                     "\"name\":\"last\"}"),
                   "the tree matches the document");
    const Node *spec = node_get(node_get(root, S("nodes"))->items[0], S("spec"));
    test_check(test, spec == node_get(root, S("base")), "an alias shares its anchor's node");

    Node *empty = parse_yaml(test, "# nothing\n");
    test_check(test, empty != nullptr && empty->kind == NODE_NULL, "an empty document is null");
    String sliced = str_slice(S("[1, 2]]"), 0, 6);
    Node *sequence = nullptr;
    test_check(test, yaml_parse(test->arena, sliced, &sequence, nullptr) == ERR_OK && sequence->count == 2,
               "the input need not be NUL-terminated");
}

static String source_of(String text, const Node *node)
{
    return str_slice(text, node->offset, node->offset + node->length);
}

static void yaml_nodes_know_where_they_are_in_the_source(Test *test)
{
    String text = S("# roles\n"
                    "stores:\n"
                    "  home:\n"
                    "    role: primary   # the one written first\n"
                    "  nas: {role: 'mirror'}\n");
    Node *root = parse_yaml(test, text.data);
    if (root == nullptr) {
        return;
    }
    const Node *stores = node_get(root, S("stores"));
    const Node *home = node_get(node_get(stores, S("home")), S("role"));
    const Node *nas = node_get(node_get(stores, S("nas")), S("role"));
    test_check(test, home->line == 4 && nas->line == 5, "each node has its line");
    test_check(test, root->line == 2 && stores->line == 3, "a block mapping, the line of its first key");
    test_check(test, node_get(stores, S("home"))->line == 4, "however deep");
    Node *list = parse_yaml(test, "items:\n\n  - a: 1\n  - b\n");
    test_check(test, list != nullptr && node_get(list, S("items"))->line == 3, "a block sequence, its first item's");
    test_check_str(test, source_of(text, home), S("primary"), "a plain scalar spans its text");
    test_check_str(test, source_of(text, nas), S("mirror"), "a quoted scalar spans what is inside its quotes");
    test_check(test, node_scalar(test->arena, NODE_STRING, S("made"))->line == 0, "a node made in code has none");
}

static void yaml_rejects_malformed_input(Test *test)
{
    const char *const invalid[] = {
        "a: [1, 2",     "\"open",      "a: 1\n b: 2\n c", "&a [*a]",       "*missing",
        "? [1]\n: x\n", "---\na\n---\nb\n", "a:\n  - b\n c: d", "{a: 1, b}: 2", "key: \"bad \\q\"",
    };
    for (size_t i = 0; i < countof(invalid); i++) {
        Node *root;
        if (yaml_parse(test->arena, S(invalid[i]), &root, nullptr) != ERR_PARSE) {
            test_check(test, false, str_cstr(test->arena, str_format(test->arena, "rejects %s", invalid[i])));
        }
    }
    Node *root;
    Err err = { 0 };
    test_check(test, yaml_parse(test->arena, S("a: 1\nb: [1, 2\n"), &root, &err) == ERR_PARSE && root == nullptr,
               "an unclosed sequence");
    test_check(test, str_starts_with(S(err.msg), S("yaml: line ")), "errors carry line and column");
    test_check(test, yaml_parse(test->arena, S("base: &x {a: *x}"), &root, &err) == ERR_PARSE, "a cycle");
    test_check_str(test, S(err.msg), S("yaml: line 1, column 14: alias inside the node it names"), "names the alias");
}

static void dotenv_reads_names_and_values(Test *test)
{
    static const char TEXT[] = "# settings\n"
                               "PORT=3000\n"
                               "export RAILS_ENV = development  # a comment\n"
                               "\r\n"
                               "QUOTED='a # b $HOME'\n"
                               "ESCAPED=\"line\\none \\\"x\\\" \\$y\"\n"
                               "EMPTY=\n"
                               "HASH=a#b\r\n";
    StringList entries = { 0 };
    Err err = { 0 };
    test_check(test, dotenv_parse(test->arena, S(TEXT), &entries, &err) == ERR_OK, err.msg);
    test_check(test, entries.count == 6, "every assignment, in order");
    if (entries.count == 6) {
        test_check_str(test, entries.items[0], S("PORT=3000"), "plain");
        test_check_str(test, entries.items[1], S("RAILS_ENV=development"), "export, spaces and a comment");
        test_check_str(test, entries.items[2], S("QUOTED=a # b $HOME"), "single quotes keep everything");
        test_check_str(test, entries.items[3], S("ESCAPED=line\none \"x\" $y"), "double quotes read escapes");
        test_check_str(test, entries.items[4], S("EMPTY="), "an empty value");
        test_check_str(test, entries.items[5], S("HASH=a#b"), "a # inside a word stays");
    }
    const char *const invalid[] = { "JUST_A_NAME\n", "1X=a\n", "A='open\n", "A=\"x\" y\n", "A=\"\\q\"\n" };
    for (size_t i = 0; i < countof(invalid); i++) {
        StringList ignored = { 0 };
        if (dotenv_parse(test->arena, S(invalid[i]), &ignored, nullptr) != ERR_PARSE) {
            test_check(test, false, str_cstr(test->arena, str_format(test->arena, "rejects %s", invalid[i])));
        }
    }
    test_check(test, dotenv_parse(test->arena, S("A=1\nB\n"), &entries, &err) == ERR_PARSE, "a line without =");
    test_check_str(test, S(err.msg), S("dotenv: line 2: expected NAME=value"), "says which line");
}

// read_all reads every record of text; each row's fields are joined with | and rows with ;.
static String read_all(Test *test, const char *text, Error *e)
{
    CsvReader reader = csv_reader_create(S(text));
    CsvRow row = { 0 };
    StringBuilder out = str_builder_create(test->arena, 64);
    bool found = false;
    while ((*e = csv_read_row(&reader, test->arena, &row, &found, nullptr)) == ERR_OK && found) {
        for (size_t i = 0; i < row.count; i++) {
            str_builder_append(&out, i == 0 ? S("") : S("|"));
            str_builder_append(&out, row.fields[i]);
        }
        str_builder_append(&out, S(";"));
    }
    return str_builder_finish(&out);
}

static void csv_reads_rfc_4180_records(Test *test)
{
    Error e = ERR_OK;
    test_check_str(test, read_all(test, "a,b\r\nc,d\r\n", &e), S("a|b;c|d;"), "CRLF records");
    test_check_str(test, read_all(test, "a,b\nc,d", &e), S("a|b;c|d;"), "LF records and no final line break");
    test_check_str(test, read_all(test, "\"x, y\",\"say \"\"hi\"\"\"\n", &e), S("x, y|say \"hi\";"),
                   "a comma and doubled quotes inside quotes");
    test_check_str(test, read_all(test, "\"line one\nline two\",z\n", &e), S("line one\nline two|z;"),
                   "a line break inside quotes");
    test_check_str(test, read_all(test, "a,,\n,\n", &e), S("a||;|;"), "empty fields");
    test_check_str(test, read_all(test, "\xEF\xBB\xBF" "Book Id,Title\n", &e), S("Book Id|Title;"),
                   "a byte order mark is skipped");
    test_check_str(test, read_all(test, "=\"0143127748\",\"\"\n", &e), S("=\"0143127748\"|;"),
                   "a quote inside an unquoted field is kept, as Goodreads writes ISBNs");
    test_check(test, e == ERR_OK, "reads them all");
}

static void csv_keeps_plain_fields_as_views(Test *test)
{
    const char *text = "plain,\"quoted\"\n";
    CsvReader reader = csv_reader_create(S(text));
    CsvRow row = { 0 };
    bool found = false;
    test_check(test, csv_read_row(&reader, test->arena, &row, &found, nullptr) == ERR_OK && found, "reads");
    test_check(test, row.fields[0].data == text && row.fields[1].data == text + 7, "both point into the text");
}

static void csv_rejects_malformed_quotes(Test *test)
{
    Err err = { 0 };
    CsvReader reader = csv_reader_create(S("ok\n\"never closed\n"));
    CsvRow row = { 0 };
    bool found = false;
    test_check(test, csv_read_row(&reader, test->arena, &row, &found, &err) == ERR_OK, "the first record");
    test_check(test, csv_read_row(&reader, test->arena, &row, &found, &err) == ERR_PARSE, "an unclosed quote");
    test_check(test, str_contains(S(err.msg), S("line 2")), "names its line");
    Error e = ERR_OK;
    read_all(test, "\"closed\"junk,x\n", &e);
    test_check(test, e == ERR_PARSE, "text after a closing quote");
}

static String render(Test *test, const char *markdown, MarkdownOptions options)
{
    String html = { 0 };
    Err err = { 0 };
    test_check(test, markdown_to_html(test->arena, S(markdown), options, &html, &err) == ERR_OK, err.msg);
    return html;
}

// GitHub's autolinks link the bare URLs and the address in these examples, which CommonMark leaves as text.
static const size_t GITHUB_AUTOLINK_EXAMPLES[] = { 608, 611, 612 };

static bool is_github_autolink_example(size_t example)
{
    for (size_t i = 0; i < countof(GITHUB_AUTOLINK_EXAMPLES); i++) {
        if (GITHUB_AUTOLINK_EXAMPLES[i] == example) {
            return true;
        }
    }
    return false;
}

// The CommonMark spec's examples follow a line of 32 backticks and " example":
// the Markdown, a line ".", the HTML, and the backticks again. A → in them
// stands for a tab. They hold raw HTML, so they render with allow_html.
static void markdown_renders_the_commonmark_spec_examples(Test *test)
{
    String spec = { 0 };
    test_check(test, file_read_all(test->arena, S("deps/md4c/test/spec.txt"), &spec, nullptr) == ERR_OK, "reads the spec");
    StringList lines = strlist_from_lines(test->arena, str_replace_all(test->arena, spec, S("→"), S("\t")));
    String fence = S("````````````````````````````````");
    String opening = str_concat(test->arena, fence, S(" example"));
    size_t examples = 0;
    for (size_t i = 0; i < lines.count; i++) {
        if (!str_equal(lines.items[i], opening)) {
            continue;
        }
        StringBuilder markdown = str_builder_create(test->arena, 256);
        for (i++; i < lines.count && !str_equal(lines.items[i], S(".")); i++) {
            str_builder_append(&markdown, lines.items[i]);
            str_builder_append_char(&markdown, '\n');
        }
        StringBuilder expected = str_builder_create(test->arena, 256);
        for (i++; i < lines.count && !str_equal(lines.items[i], fence); i++) {
            str_builder_append(&expected, lines.items[i]);
            str_builder_append_char(&expected, '\n');
        }
        examples++;
        String html = render(test, str_builder_finish(&markdown).data, (MarkdownOptions){ .allow_html = true });
        const char *what = str_format(test->arena, "example %zu", examples).data;
        if (is_github_autolink_example(examples)) {
            test_check(test, str_contains(html, S("<a href=\"")), what);
        } else {
            test_check_str(test, html, str_builder_finish(&expected), what);
        }
    }
    test_check(test, examples == 652, "finds all 652 examples");
}

static void markdown_renders_github_extensions(Test *test)
{
    MarkdownOptions options = { 0 };
    test_check_str(test, render(test, "| a | b |\n|:--|--:|\n| 1 | 2 |\n", options),
                   S("<table>\n<thead>\n<tr>\n<th align=\"left\">a</th>\n<th align=\"right\">b</th>\n</tr>\n</thead>\n"
                     "<tbody>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"right\">2</td>\n</tr>\n</tbody>\n</table>\n"),
                   "a table with aligned columns");
    test_check_str(test, render(test, "- [ ] todo\n- [x] done\n", options),
                   S("<ul>\n<li><input disabled=\"\" type=\"checkbox\"> todo</li>\n"
                     "<li><input checked=\"\" disabled=\"\" type=\"checkbox\"> done</li>\n</ul>\n"),
                   "a task list");
    test_check_str(test, render(test, "~~gone~~", options), S("<p><del>gone</del></p>\n"), "strikethrough");
    test_check_str(test, render(test, "see www.example.com, https://example.org/a or me@example.com", options),
                   S("<p>see <a href=\"http://www.example.com\">www.example.com</a>, <a href=\"https://example.org/a\">"
                     "https://example.org/a</a> or <a href=\"mailto:me@example.com\">me@example.com</a></p>\n"),
                   "links without angle brackets");
    test_check_str(test, render(test, "\xEF\xBB\xBF# Title", options), S("<h1>Title</h1>\n"), "a byte order mark is skipped");
}

static void markdown_keeps_html_and_scripts_out(Test *test)
{
    MarkdownOptions options = { 0 };
    test_check_str(test, render(test, "<script>alert(1)</script>", options),
                   S("<p>&lt;script&gt;alert(1)&lt;/script&gt;</p>\n"), "a raw HTML block is text");
    test_check_str(test, render(test, "a <b onclick=\"go()\">b</b>", options),
                   S("<p>a &lt;b onclick=&quot;go()&quot;&gt;b&lt;/b&gt;</p>\n"), "inline raw HTML is text");
    test_check_str(test, render(test, "[a](javascript:alert(1))", options), S("<p><a href=\"\">a</a></p>\n"),
                   "a javascript: link");
    test_check_str(test, render(test, "[a](JaVaScRiPt&#x3A;alert(1))", options), S("<p><a href=\"\">a</a></p>\n"),
                   "a javascript: link in mixed case behind an entity");
    test_check_str(test, render(test, "![a](data:text/html;base64,PHNjcmlwdD4=)", options),
                   S("<p><img src=\"\" alt=\"a\" /></p>\n"), "a data: URL that is no image");
    test_check_str(test, render(test, "![a](data:image/png;base64,iVBO)", options),
                   S("<p><img src=\"data:image/png;base64,iVBO\" alt=\"a\" /></p>\n"), "a PNG data: image");
    test_check_str(test, render(test, "a <b>b</b>\n\n[a](javascript:go())", (MarkdownOptions){ .allow_html = true }),
                   S("<p>a <b>b</b></p>\n<p><a href=\"javascript:go()\">a</a></p>\n"), "allow_html passes both");
}

// xml_outline writes the events of text as <name a="v"> for a start, </name> for an end and [text] for text.
static String xml_outline(Test *test, String text)
{
    XmlEvents events = { 0 };
    Err err = { 0 };
    test_check(test, xml_parse(test->arena, text, &events, &err) == ERR_OK, err.msg);
    StringBuilder out = str_builder_create(test->arena, 64);
    for (size_t i = 0; i < events.count; i++) {
        const XmlEvent *event = &events.items[i];
        switch (event->kind) {
        case XML_EVENT_START:
            str_builder_append_format(&out, "<%s", event->name.data);
            for (size_t a = 0; a < event->attribute_count; a++) {
                str_builder_append_format(&out, " %s=\"%s\"", event->attributes[a].name.data,
                                          event->attributes[a].value.data);
            }
            str_builder_append_char(&out, '>');
            break;
        case XML_EVENT_END:
            str_builder_append_format(&out, "</%s>", event->name.data);
            break;
        case XML_EVENT_TEXT:
            str_builder_append_format(&out, "[%s]", event->text.data);
            break;
        }
    }
    return str_builder_finish(&out);
}

static void xml_reads_elements_attributes_and_text(Test *test)
{
    test_check_str(test,
                   xml_outline(test, S("<?xml version=\"1.0\"?>\n<feed lang=\"no\"><title>Bøker &amp; blader</title>"
                                       "<!-- note --><entry id='1'/></feed>")),
                   S("<feed lang=\"no\"><title>[Bøker & blader]</title><entry id=\"1\"></entry></feed>"),
                   "elements, attributes and decoded text, without the declaration and comments");
    test_check_str(test, xml_outline(test, S("<a>one<![CDATA[ <two> ]]>three&#x21;</a>")), S("<a>[one <two> three!]</a>"),
                   "text in pieces comes as one event");
    test_check_str(test,
                   xml_outline(test, S("<rss xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:creator>Ibsen</dc:creator></rss>")),
                   S("<rss xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:creator>[Ibsen]</dc:creator></rss>"),
                   "names keep their prefix");
    test_check_str(test,
                   xml_outline(test, S("<!DOCTYPE a [<!ENTITY who \"Nora\"><!ATTLIST a kind CDATA \"play\">]><a>&who;</a>")),
                   S("<a kind=\"play\">[Nora]</a>"), "entities and attribute defaults from the DTD");
    const char utf16[] = "\xFF\xFE<\0a\0>\0\xE6\0<\0/\0a\0>\0";
    test_check_str(test, xml_outline(test, (String){ utf16, sizeof utf16 - 1 }), S("<a>[æ]</a>"),
                   "UTF-16 with a byte order mark");
    test_check_str(test, xml_outline(test, S("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><a>\xE6</a>")),
                   S("<a>[æ]</a>"), "ISO-8859-1 as declared");

    XmlEvents events = { 0 };
    test_check(test, xml_parse(test->arena, S("<a>\n  <b/>\n</a>"), &events, nullptr) == ERR_OK && events.count == 6 &&
                         events.items[2].line == 2 && events.items[5].line == 3,
               "events know their line");
}

static void xml_rejects_malformed_and_hostile_documents(Test *test)
{
    XmlEvents events = { 0 };
    Err err = { 0 };
    test_check(test, xml_parse(test->arena, S("<a>\n<b></a>"), &events, &err) == ERR_PARSE, "a mismatched tag");
    test_check_str(test, S(err.msg), S("xml: line 2, column 6: mismatched tag"), "names the line and column");
    test_check(test, xml_parse(test->arena, S(""), &events, &err) == ERR_PARSE, "no element at all");

    // Expat checks the amplification once 8 MiB have come out, so a long lol0 gets there in few expansions,
    // which keeps the test fast under gcc's sanitizers.
    StringBuilder laughs = str_builder_create(test->arena, 4096);
    str_builder_append_format(&laughs, "<!DOCTYPE lolz [<!ENTITY lol0 \"%s\">", str_repeat(test->arena, S("lol"), 341).data);
    for (int i = 1; i <= 9; i++) {
        str_builder_append_format(&laughs, "<!ENTITY lol%d \"", i);
        for (int j = 0; j < 10; j++) {
            str_builder_append_format(&laughs, "&lol%d;", i - 1);
        }
        str_builder_append(&laughs, S("\">"));
    }
    str_builder_append(&laughs, S("]><lolz>&lol9;</lolz>"));
    test_check(test, xml_parse(test->arena, str_builder_finish(&laughs), &events, &err) == ERR_PARSE,
               "billion laughs, a billion copies of lol from a page of entities");
    test_check(test, str_contains(S(err.msg), S("amplification")), err.msg);

    test_check(test,
               xml_parse(test->arena, S("<!DOCTYPE a [<!ENTITY e SYSTEM \"file:///etc/passwd\">]><a>&e;</a>"), &events,
                         &err) == ERR_OK &&
                   events.count == 2,
               "an external entity is never loaded");
}

const TestCase ENCODING_TESTS[] = {
    { "json_parses_every_kind", json_parses_every_kind },
    { "json_decodes_strings", json_decodes_strings },
    { "json_rejects_malformed_input", json_rejects_malformed_input },
    { "json_encodes_and_quotes", json_encodes_and_quotes },
    { "json_reads_numbers_of_any_length", json_reads_numbers_of_any_length },
    { "json_keeps_only_the_tree_and_buffer", json_keeps_only_the_tree_and_buffer },
    { "json_floats_ignore_the_locale", json_floats_ignore_the_locale },
    { "yaml_types_scalars_by_the_core_schema", yaml_types_scalars_by_the_core_schema },
    { "yaml_reads_block_and_flow_collections", yaml_reads_block_and_flow_collections },
    { "yaml_nodes_know_where_they_are_in_the_source", yaml_nodes_know_where_they_are_in_the_source },
    { "yaml_rejects_malformed_input", yaml_rejects_malformed_input },
    { "dotenv_reads_names_and_values", dotenv_reads_names_and_values },
    { "csv_reads_rfc_4180_records", csv_reads_rfc_4180_records },
    { "csv_keeps_plain_fields_as_views", csv_keeps_plain_fields_as_views },
    { "csv_rejects_malformed_quotes", csv_rejects_malformed_quotes },
    { "markdown_renders_the_commonmark_spec_examples", markdown_renders_the_commonmark_spec_examples },
    { "markdown_renders_github_extensions", markdown_renders_github_extensions },
    { "markdown_keeps_html_and_scripts_out", markdown_keeps_html_and_scripts_out },
    { "xml_reads_elements_attributes_and_text", xml_reads_elements_attributes_and_text },
    { "xml_rejects_malformed_and_hostile_documents", xml_rejects_malformed_and_hostile_documents },
    { nullptr, nullptr },
};
