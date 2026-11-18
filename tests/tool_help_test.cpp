// tests/tool_help_test.cpp — the operator's tool help: a purpose line that says something, a usage line in the slash
// syntax /<server> reads, and a parameter table with types, defaults and meanings, all read off the tool's schema.
#include "dd/tools/tool_help.hpp"

#include <cstdio>
#include <string>

using dd::core::Json;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}
static bool has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int main() {
    const Json research = dd::core::json_parse(R"({
        "type": "object",
        "properties": {
            "topic": {"type": "string", "description": "what to research; job words, not provider names"},
            "depth": {"type": "string", "enum": ["quick", "default", "deep"], "default": "deep"},
            "n": {"type": "integer", "default": 20, "description": "candidates to return"},
            "wurm": {"type": "boolean", "description": "deep dig | follow every finding"},
            "sources": {"type": "array", "items": {"type": "string"}}
        },
        "required": ["topic"]
    })");
    const std::string desc = "DEFAULT for /pulse <query>. Runs the full ratnest dive: search, wurm-dig, re-seed, dig "
                             "deeper, stop when growth plateaus, filter the final pool, return the top N. One call.";

    {   // a first sentence that says nothing takes the next one
        const std::string p = dd::tools::tool_purpose(desc);
        chk(p.rfind("DEFAULT for /pulse <query>. Runs the full ratnest dive", 0) == 0, "purpose: the second sentence joins a short first");
        chk(p.size() <= 140 + 3, "purpose: clipped to the width");
        chk(dd::tools::tool_purpose("List one folder, one level deep and sorted by name. It does not search.") == "List one folder, one level deep and sorted by name.",
            "purpose: a sentence that says something stands alone");
        chk(dd::tools::tool_purpose("Look  at\n an image.") == "Look at an image.", "purpose: one line");
    }
    {   // usage: the slash syntax, from the schema
        chk(dd::tools::usage_synopsis(&research, false) == "<topic> [options]", "usage: required words, options marked");
        chk(dd::tools::usage_synopsis(&research, true) == "<topic> [--depth quick|default|deep] [--n N] [--wurm] [--sources a,b]",
            "usage: every flag spelled out with what it takes");
        const Json call = dd::core::json_parse(R"({"type":"object","properties":{"tool":{"type":"string"},"args":{"type":"object"}},"required":["tool","args"]})");
        chk(dd::tools::usage_synopsis(&call, false) == "<tool> --args '{…}'", "usage: a second required field is a flag");
        chk(dd::tools::usage_synopsis(nullptr, true).empty(), "usage: no schema, no line");
    }
    {   // the index: purpose and usage per tool
        dd::tools::ToolDoc d;
        d.name = "research_start";
        d.wire = "pulse_research_start";
        d.toolset = "pulse";
        d.description = desc;
        d.params = &research;
        dd::tools::ToolDoc bare;
        bare.name = "health";
        const std::string md = dd::tools::tools_index_md("/pulse · 2 tools", {d, bare}, "/pulse");
        chk(has(md, "### /pulse · 2 tools"), "index: titled");
        chk(has(md, "| tool | what it is for | usage |"), "index: a usage column for a slash server");
        chk(has(md, "| `research_start` | DEFAULT for /pulse <query>. Runs the full ratnest dive"), "index: the purpose");
        chk(has(md, "`/pulse research_start <topic> [options]`"), "index: how to call it from here");
        chk(has(md, "| `health` | (the server gives no description) | `/pulse health` |"), "index: a silent server says so");
        const std::string plain = dd::tools::tools_index_md("built-in · 1", {bare}, "");
        chk(has(plain, "| tool | what it is for |\n|---|---|"), "index: no usage column for built-ins");

        const std::string page = dd::tools::tool_detail_md(d, "/pulse");
        chk(has(page, "### `research_start` · pulse"), "detail: name and where it comes from");
        chk(has(page, desc), "detail: the whole description");
        chk(has(page, "**Usage** `/pulse research_start <topic> [--depth quick|default|deep] [--n N] [--wurm] [--sources a,b]`"),
            "detail: the full usage");
        chk(has(page, "**The model calls it** `pulse_research_start`"), "detail: the wire name when it differs");
        chk(has(page, "| `topic` | string | **required** |  | what to research; job words, not provider names |"), "detail: a required row");
        chk(has(page, "| `depth` | quick · default · deep |  | `deep` |"), "detail: an enum's values and its default");
        chk(has(page, "deep dig \\| follow every finding"), "detail: a pipe in a description stays in its cell");
        chk(has(page, "| `sources` | list of string |"), "detail: a list's item type");
        chk(has(dd::tools::tool_detail_md(bare, ""), "No parameters."), "detail: a tool without parameters says so");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("tool help: all passed\n");
    return g_fail ? 1 : 0;
}
