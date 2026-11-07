// tests/tools_wire_shape_test.cpp — the wire contract every model-facing
// request is checked against.
//
// A strict provider (OpenRouter is one) deserializes each entry of the
// `tools` array into a typed target:
//      {"type":"function","function":{name, description, parameters}}
// with `parameters` a REQUIRED object. A registration that hands the bare
// JSON-Schema inline puts the schema's own keys inside `function` and leaves
// the target without `parameters`; the provider then answers HTTP 200 with
//      Invalid JSON data: Failed to deserialize the JSON body into the
//      target type: tools[13].function: missing field `parameters`
// and the whole request is refused — the turn dies before the model sees
// anything, and the message reads like a wire bug rather than one bad
// registration. The contract is asserted here at the registry seam, where
// every emitter gets its tool list from, not at whichever provider one
// session happens to point at.
#include "dd/tools/registry.hpp"
#include "dd/core/json.hpp"

#include <cstdio>
#include <iostream>
#include <set>
#include <string>
#include <vector>

using namespace dd;
using namespace dd::tools;
using dd::core::Json;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::cerr << "FAIL: " #cond "\n"; } } while (0)
#define CHECK2(cond, label) do { if (!(cond)) { ++failures; \
    std::cerr << "FAIL: " #cond " — " label "\n"; } } while (0)

// Read the definition the way a strict server reads it.
static bool fn_well_formed(const Json& wrapper) {
    const Json* type = core::json_get(wrapper, "type");
    const Json* fn = core::json_get(wrapper, "function");
    if (!type || type->type != Json::Str || type->str != "function") return false;
    if (!fn || fn->type != Json::Obj) return false;
    if (!core::json_get(*fn, "name")) return false;
    const Json* params = core::json_get(*fn, "parameters");
    return params && params->type == Json::Obj;
}

// The schema keys a strict target has nowhere to put inside `function`.
static bool fn_leaks_schema_keys(const Json& wrapper) {
    static const std::set<std::string> leakable{"type", "properties", "required",
                                               "additionalProperties"};
    const Json* fn = core::json_get(wrapper, "function");
    if (!fn) return true;
    for (const auto& [k, sp] : fn->obj)
        if (leakable.count(k)) return true;
    return false;
}

// A one-argument JSON-Schema, built in steps so the nesting cannot drift.
static Json one_string_property(const std::string& arg) {
    Json prop = core::json_obj();
    core::obj_set(prop, "type", core::json_str("string"));
    Json props = core::json_obj();
    core::obj_set(props, arg, prop);
    Json required = core::json_arr({std::make_shared<Json>(core::json_str(arg))});
    Json schema = core::json_obj();
    core::obj_set(schema, "type", core::json_str("object"));
    core::obj_set(schema, "properties", props);
    core::obj_set(schema, "required", required);
    return schema;
}

int main() {
    ToolRegistry reg;

    // 1. A genuinely parameterless tool: the schema omits `parameters`.
    {
        ToolEntry plain;
        plain.name = "now";
        plain.toolset = "core";
        plain.schema = core::json_obj();
        core::obj_set(plain.schema, "description", core::json_str("Read the wall clock"));
        plain.handler = [](const Json&) { return std::string("ok"); };
        reg.register_tool(plain);
    }

    // 2. The defect: the schema handed inline, so type/properties/required
    //    sit inside `function` where `parameters` belongs. This is what
    //    vision_tool_schema() used to return, and what put view_image at
    //    tools[13] in the body OpenRouter refused.
    {
        ToolEntry inline_tool;
        inline_tool.name = "inline_schema";
        inline_tool.toolset = "core";
        inline_tool.schema = core::json_obj();
        core::obj_set(inline_tool.schema, "description", core::json_str("Takes one path"));
        Json bare = one_string_property("path");
        for (const auto& [k, sp] : bare.obj)
            core::obj_set(inline_tool.schema, k, *sp);   // spread it, no wrapper
        inline_tool.handler = [](const Json&) { return std::string("ok"); };
        reg.register_tool(inline_tool);
    }

    // 3. A malformed `parameters`: present, but not an object.
    {
        ToolEntry broken;
        broken.name = "broken_params";
        broken.toolset = "core";
        broken.schema = core::json_obj();
        core::obj_set(broken.schema, "description", core::json_str("parameters is a string"));
        core::obj_set(broken.schema, "parameters", core::json_str("oops"));
        broken.handler = [](const Json&) { return std::string("ok"); };
        reg.register_tool(broken);
    }

    // 4. A registration already in the shape the target expects: it must
    //    reach the wire unchanged, not re-wrapped or emptied.
    {
        ToolEntry good;
        good.name = "good_shape";
        good.toolset = "core";
        good.schema = core::json_obj();
        core::obj_set(good.schema, "description", core::json_str("Takes a query"));
        Json params = one_string_property("query");
        core::obj_set(good.schema, "parameters", params);
        good.handler = [](const Json&) { return std::string("ok"); };
        reg.register_tool(good);
    }

    const std::vector<std::string> names{"now", "inline_schema", "broken_params", "good_shape"};
    const std::vector<Json> defs = reg.get_definitions(names, true);
    CHECK2(defs.size() == names.size(), "every registered tool reaches the wire");

    for (const auto& w : defs) {
        CHECK2(fn_well_formed(w), "the function object the target type accepts");
        CHECK2(!fn_leaks_schema_keys(w), "no schema key sits at the function level");
    }

    // The promoted case must keep its real signature: blanking `parameters`
    // would have satisfied the server while declaring a tool that takes
    // nothing, so the argument stays declared where it can be called.
    for (const auto& w : defs) {
        const Json* fn = core::json_get(w, "function");
        const Json* nm = core::json_get(*fn, "name");
        if (!nm || nm->str != "inline_schema") continue;
        const Json* params = core::json_get(*fn, "parameters");
        const Json* props = core::json_get(*params, "properties");
        const Json* req = core::json_get(*params, "required");
        CHECK2(props && core::json_get(*props, "path"), "the promoted schema still declares 'path'");
        CHECK2(req && req->type == Json::Arr && !req->arr.empty(), "'required' moved with it");
        CHECK2(core::json_get(*fn, "properties") == nullptr,
               "'properties' left the function object");
        std::cerr << "[info] promoted inline schema on the wire: "
                  << core::json_dump(w) << "\n";
    }

    // The parameterless case keeps a callable, empty signature.
    for (const auto& w : defs) {
        const Json* fn = core::json_get(w, "function");
        const Json* nm = core::json_get(*fn, "name");
        if (!nm || nm->str != "now") continue;
        const Json* params = core::json_get(*fn, "parameters");
        const Json* ptype = core::json_get(*params, "type");
        const Json* props = core::json_get(*params, "properties");
        CHECK2(ptype && ptype->str == "object", "a parameterless tool defaults to an object schema");
        CHECK2(props && props->type == Json::Obj && props->obj.empty(),
               "with an empty properties object");
    }

    // A correct registration survives: its parameters is still its own.
    for (const auto& w : defs) {
        const Json* fn = core::json_get(w, "function");
        const Json* nm = core::json_get(*fn, "name");
        if (!nm || nm->str != "good_shape") continue;
        const Json* params = core::json_get(*fn, "parameters");
        const Json* props = core::json_get(*params, "properties");
        CHECK2(props && core::json_get(*props, "query"), "a correct schema is left alone");
    }

    if (failures) std::fprintf(stderr, "%d wire-shape checks failed\n", failures);
    else std::fprintf(stderr, "all wire-shape tests passed\n");
    return failures ? 1 : 0;
}
