#include "dd/tools/registry.hpp"
#include "dd/core/json.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace dd::tools;
using dd::core::Json;

using namespace dd;
using namespace dd::tools;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::cerr << "FAIL: " #cond "\n"; } } while (0)

int main() {
    ToolRegistry reg;

    // A tool whose check passes.
    ToolEntry echo;
    echo.name = "echo";
    echo.toolset = "core";
    echo.schema = core::json_obj({{"description", std::make_shared<Json>(core::json_str("Echo input"))}});
    echo.handler = [](const Json& args) {
        const Json* msg = core::json_get(args, "msg");
        return msg ? msg->str : std::string("{}");
    };
    echo.check_fn = [] { return true; };
    reg.register_tool(echo);

    // A tool whose check fails (unavailable).
    ToolEntry secret;
    secret.name = "secret";
    secret.toolset = "premium";
    secret.schema = core::json_obj({{"description", std::make_shared<Json>(core::json_str("Needs env"))}});
    secret.handler = [](const Json&) { return std::string("ok"); };
    secret.check_fn = [] { return false; };
    secret.requires_env = {"SECRET_TOKEN"};
    reg.register_tool(secret);

    // A tool with no check (always available).
    ToolEntry plain;
    plain.name = "plain";
    plain.toolset = "core";
    plain.schema = core::json_obj({{"description", std::make_shared<Json>(core::json_str("No check"))}});
    plain.handler = [](const Json&) { return std::string("plain-result"); };
    reg.register_tool(plain);

    // --- dispatch ---
    Json arg = core::json_obj({{"msg", std::make_shared<Json>(core::json_str("hello"))}});
    CHECK(reg.dispatch("echo", arg) == "hello");
    CHECK(reg.dispatch("plain", Json{}) == "plain-result");
    // unknown tool -> {"error":"Unknown tool: nope"}
    CHECK(reg.dispatch("nope", Json{}).find("Unknown tool: nope") != std::string::npos);

    // --- get_definitions: secret filtered out (check false), echo+plain kept ---
    auto defs = reg.get_definitions({"echo", "secret", "plain"});
    CHECK(defs.size() == 2);
    bool saw_echo = false, saw_plain = false, saw_secret = false;
    for (const auto& d : defs) {
        auto dumped = core::json_dump(d);
        if (dumped.find("\"echo\"") != std::string::npos) saw_echo = true;
        if (dumped.find("\"plain\"") != std::string::npos) saw_plain = true;
        if (dumped.find("\"secret\"") != std::string::npos) saw_secret = true;
    }
    CHECK(saw_echo && saw_plain && !saw_secret);

    // --- the wire contract: every model-facing `function` carries a valid
    //     `parameters` object. None of these tools declared one, so an
    //     un-repaired schema would put a bare {description, name} inside
    //     `function` and a strict server would answer the whole request with
    //     "tools[N].function: missing field parameters" — a refused body, so
    //     the turn dies before the model sees anything. ---
    for (const auto& d : defs) {
        const std::string s = core::json_dump(d);
        CHECK(s.find("\"parameters\"") != std::string::npos);
        const Json* fn = core::json_get(d, "function");
        const Json* params = fn ? core::json_get(*fn, "parameters") : nullptr;
        CHECK(params && params->type == Json::Obj);
    }

    // --- a schema that sets `parameters` to the wrong kind is repaired too,
    //     so the invariant holds whatever a registration hands in ---
    {
        ToolRegistry reg2;
        ToolEntry broken;
        broken.name = "broken";
        broken.toolset = "core";
        broken.schema = core::json_obj({{"description", std::make_shared<Json>(core::json_str("bad parameters"))},
                                        {"parameters", std::make_shared<Json>(core::json_str("not-an-object"))}});
        broken.handler = [](const Json&) { return std::string("ok"); };
        reg2.register_tool(broken);
        auto bdefs = reg2.get_definitions({"broken"});
        CHECK(bdefs.size() == 1);
        const Json* bfn = bdefs.empty() ? nullptr : core::json_get(bdefs[0], "function");
        const Json* bparams = bfn ? core::json_get(*bfn, "parameters") : nullptr;
        CHECK(bparams && bparams->type == Json::Obj);
        CHECK(bparams && bparams->obj.size() == 2);   // {"type":"object","properties":{}}
    }

    // --- availability ---
    CHECK(reg.is_toolset_available("core"));
    CHECK(!reg.is_toolset_available("premium"));
    auto req = reg.check_toolset_requirements();
    CHECK(req["core"] == true && req["premium"] == false);
    auto avail = reg.get_available_toolsets();
    CHECK(avail.size() == 1 && avail[0] == "core");

    // --- max result size ---
    CHECK(reg.get_max_result_size("echo") == 100000);  // global default
    ToolEntry capped = echo; capped.name = "capped"; capped.max_result_size_chars = 512;
    reg.register_tool(capped);
    CHECK(reg.get_max_result_size("capped") == 512);

    // --- aliases: only the alias name itself resolves; a wire name never borrows
    //     a toolset because it starts with one ("pulse_x" is not an x_ tool) ---
    reg.register_toolset_alias("core_ext", "core");
    CHECK(reg.get_toolset_alias_target("core_ext") == "core");
    CHECK(reg.get_toolset_for_tool("core_ext") == "core");           // the alias itself
    CHECK(reg.get_toolset_for_tool("core_ext_foo").empty());         // no prefix guessing

    // --- names / schema / emoji ---
    auto names = reg.get_all_tool_names();
    CHECK(names.size() == 4);
    CHECK(reg.get_emoji("echo") == "\xE2\x9A\xA1");  // default lightning
    CHECK(reg.get_schema("echo") != nullptr);

    // --- deregister cleans up toolset check when last tool in set removed ---
    reg.deregister("secret");
    CHECK(reg.generation() > 0);
    CHECK(reg.is_toolset_available("premium"));  // no check now -> available
    if (failures == 0)
        std::cout << "all tools-registry tests passed\n";
    return failures == 0 ? 0 : 1;
}
