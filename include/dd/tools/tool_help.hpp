#pragma once
// dd/tools/tool_help.hpp — what a tool is for and how to call it, read off its own schema.
//
// The deck listed tools as bare names ("pulse_research, pulse_research_start, …"). These turn a tool's schema (the
// description and parameters the model sees) into the operator's help: a purpose line per tool in an index, and a
// page per tool with every parameter's type, default and meaning. Markdown, for the deck's doc card. The usage line
// is the slash syntax /<server> reads (dd/tools/slash_args.hpp): words fill the first required string, --name value
// sets the rest.

#include <cstddef>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

struct ToolDoc {
    std::string name;                        // as the deck calls it (a bare alias under /<server>)
    std::string wire;                        // as the model calls it
    std::string toolset;                     // "built-in" or the MCP server's config name
    std::string description;
    const dd::core::Json* params = nullptr;  // a JSON Schema object: properties, required
};

// One line: what the tool is for. The first sentence, or two when the first says nothing ("DEFAULT for /pulse.").
std::string tool_purpose(const std::string& description, std::size_t max = 140);

// "<topic> --job_id TEXT [options]" — `full` spells every optional flag out instead of [options].
std::string usage_synopsis(const dd::core::Json* params, bool full);

// A table of tools: name, purpose and (with a slash prefix such as "/pulse") the usage column.
std::string tools_index_md(const std::string& title, const std::vector<ToolDoc>& tools, const std::string& slash);

// Everything about one tool: description, usage, and a table of its parameters.
std::string tool_detail_md(const ToolDoc& tool, const std::string& slash);

}  // namespace tools
}  // namespace dd
