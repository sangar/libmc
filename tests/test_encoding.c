#include "test.h"

#include <locale.h>
#include <math.h>

#include "mc/encoding/json.h"
#include "mc/encoding/yaml.h"

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
    Node *root = parse(test, "[\"tab\\tquote\\\"slash\\/\", \"\\u00e6\\ud83d\\ude00\", \"\\ud800x\", \"\"]");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, root->items[0]->text, S("tab\tquote\"slash/"), "simple escapes");
    test_check_str(test, root->items[1]->text, S("æ😀"), "\\u escapes and a surrogate pair");
    test_check_str(test, root->items[2]->text, S("\xEF\xBF\xBDx"), "a lone surrogate becomes U+FFFD");
    test_check(test, root->items[3]->text.len == 0, "the empty string");
}

static void json_rejects_malformed_input(Test *test)
{
    const char *const invalid[] = {
        "",          "[1,]",      "{\"a\":1,}", "{'a':1}", "01",      "1.",       ".5",    "-",
        "1e",        "\"\\x\"",   "\"open",    "[1 2]",   "{\"a\" 1}", "tru",     "nul",   "\"\x01\"",
        "\"\xff\"",  "1 2",       "1e400",     "[",       "{\"a\":}", "\"\\u12\"", "NaN",
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
    test_check_str(test, S(err.msg), S("json: line 2, column 8: expected a value"), "errors carry line and column");
}

static void json_encodes_and_quotes(Test *test)
{
    Node *root = parse(test, "{\"a\": [1, -0.1, 1e300, \"x\\ny\"], \"b\": {\"c\": null, \"d\": false}}");
    if (root == nullptr) {
        return;
    }
    test_check_str(test, json_encode(test->arena, root),
                   S("{\"a\":[1,-0.1,1e+300,\"x\\ny\"],\"b\":{\"c\":null,\"d\":false}}"), "compact output");
    Node *nan = node_create(test->arena, NODE_FLOAT);
    nan->number = NAN;
    test_check_str(test, json_encode(test->arena, nan), S("null"), "not a number becomes null");
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
                     "\"floats\":[1.5,-2e+03,null,null,9.223372036854776e+18],"
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

const TestCase ENCODING_TESTS[] = {
    { "json_parses_every_kind", json_parses_every_kind },
    { "json_decodes_strings", json_decodes_strings },
    { "json_rejects_malformed_input", json_rejects_malformed_input },
    { "json_encodes_and_quotes", json_encodes_and_quotes },
    { "json_reads_numbers_of_any_length", json_reads_numbers_of_any_length },
    { "json_floats_ignore_the_locale", json_floats_ignore_the_locale },
    { "yaml_types_scalars_by_the_core_schema", yaml_types_scalars_by_the_core_schema },
    { "yaml_reads_block_and_flow_collections", yaml_reads_block_and_flow_collections },
    { "yaml_rejects_malformed_input", yaml_rejects_malformed_input },
    { nullptr, nullptr },
};
