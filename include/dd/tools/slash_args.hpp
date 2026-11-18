#pragma once
// dd/tools/slash_args.hpp — "/<server> <tool> <text>" as a tool call's arguments, read off the tool's own schema.
//
// The text is a JSON object (passed through as typed), or words and flags. Words fill the tool's primary property:
// its first required string, else query/topic. `--name value` sets a declared property by its type, `--name` alone
// sets a boolean, `--no-name` clears one, `--value` sets the one enum property that lists it, `-x v` is `--x v`.
// A flag the schema does not declare stays in the text, so nothing the operator typed is dropped. No schema: the
// whole text is {"query": text}, the shape this command always sent.

#include <string>

#include "dd/core/json.hpp"

namespace dd {
namespace tools {

dd::core::Json slash_args(const std::string& text, const dd::core::Json* input_schema);

}  // namespace tools
}  // namespace dd
