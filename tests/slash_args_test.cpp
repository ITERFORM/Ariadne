// tests/slash_args_test.cpp — "/<server> <tool> <text>": words land in the tool's own property, flags are typed by its
// schema, a flag the schema does not declare stays in the text, and JSON passes through.
#include "dd/tools/slash_args.hpp"

#include <cstdio>
#include <string>

using dd::core::Json;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

static std::string str(const Json& o, const char* k) {
    const Json* v = dd::core::json_get(o, k);
    return v && v->type == Json::Str ? v->str : "<none>";
}
static double num(const Json& o, const char* k) {
    const Json* v = dd::core::json_get(o, k);
    return v && v->type == Json::Num ? v->num : -1;
}
static int boolean(const Json& o, const char* k) {
    const Json* v = dd::core::json_get(o, k);
    return v && v->type == Json::Bool ? (v->b ? 1 : 0) : -1;
}

int main() {
    // the pulse research tool, as its manifest declares it
    const Json schema = dd::core::json_parse(R"({
        "type": "object",
        "properties": {
            "topic": {"type": "string"},
            "depth": {"type": "string", "enum": ["quick", "default", "deep"]},
            "sources": {"type": "array", "items": {"type": "string"}},
            "n": {"type": "integer"},
            "max_wurm_rounds": {"type": "integer"},
            "wurm": {"type": "boolean"},
            "llm_filter": {"type": "boolean"},
            "anchor": {"type": "string", "enum": ["auto", "strict", "off"]}
        },
        "required": ["topic"]
    })");

    {   // the call that came back 422: words to topic, flags to their properties
        const Json a = dd::tools::slash_args("XRP ledger SWIFT QNT --deep --wurm --n 100", &schema);
        chk(str(a, "topic") == "XRP ledger SWIFT QNT", "words fill the required property, not a made-up query");
        chk(!dd::core::json_get(a, "query"), "no query key is invented");
        chk(str(a, "depth") == "deep", "--deep sets the enum property that lists it");
        chk(boolean(a, "wurm") == 1, "--wurm sets the boolean");
        chk(num(a, "n") == 100, "--n 100 is an integer");
    }
    {
        const Json a = dd::tools::slash_args("\"kv cache paging\" --sources reddit,hackernews --max-wurm-rounds=2 --no-llm-filter -n 5", &schema);
        chk(str(a, "topic") == "kv cache paging", "a quoted topic is one value");
        const Json* s = dd::core::json_get(a, "sources");
        chk(s && s->type == Json::Arr && s->arr.size() == 2 && s->arr[1]->str == "hackernews", "--sources a,b is an array");
        chk(num(a, "max_wurm_rounds") == 2, "--max-wurm-rounds=2: dashes match underscores, = carries the value");
        chk(boolean(a, "llm_filter") == 0, "--no-llm-filter clears the boolean");
        chk(num(a, "n") == 5, "-n 5 is --n 5");
    }
    {   // nothing the operator typed is dropped
        const Json a = dd::tools::slash_args("rust async --bogus thing --n many", &schema);
        chk(str(a, "topic") == "rust async --bogus thing --n many", "undeclared flags and untyped values stay in the text");
        chk(!dd::core::json_get(a, "n"), "a value that is not an integer sets nothing");
    }
    chk(boolean(dd::tools::slash_args("x --wurm false", &schema), "wurm") == 0, "--wurm false reads the word");
    chk(str(dd::tools::slash_args("don't panic", &schema), "topic") == "don't panic", "an apostrophe inside a word is a letter");
    {   // JSON passes through untouched
        const Json a = dd::tools::slash_args(R"({"topic": "as typed", "n": 3})", &schema);
        chk(str(a, "topic") == "as typed" && num(a, "n") == 3, "a JSON object passes through");
    }
    {   // no schema: the old one-arg shape
        const Json a = dd::tools::slash_args("hello world", nullptr);
        chk(str(a, "query") == "hello world", "no schema: {\"query\": text}");
        chk(dd::tools::slash_args("   ", nullptr).obj.empty(), "empty text: no arguments");
    }
    {   // a tool whose one-arg name is query keeps query
        const Json q = dd::core::json_parse(R"({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"integer"}}})");
        const Json a = dd::tools::slash_args("memory graph --limit 3", &q);
        chk(str(a, "query") == "memory graph" && num(a, "limit") == 3, "a query tool still gets query");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("slash args: all passed\n");
    return g_fail ? 1 : 0;
}
