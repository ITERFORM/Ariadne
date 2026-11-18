#include "dd/tools/tool_help.hpp"

#include <cctype>

namespace dd {
namespace tools {
namespace {

using dd::core::Json;

std::string squash(const std::string& s) {   // one line: whitespace runs to single spaces
    std::string o;
    bool space = false;
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            space = !o.empty();
            continue;
        }
        if (space) o += ' ';
        space = false;
        o += c;
    }
    return o;
}

std::string cell(const std::string& s) {   // a table cell: one line, a pipe is a character
    std::string o;
    for (char c : squash(s)) {
        if (c == '|') o += '\\';
        o += c;
    }
    return o;
}

std::string clip(std::string s, std::size_t max) {
    if (s.size() <= max) return s;
    std::size_t cut = max;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;   // never inside a codepoint
    const std::size_t space = s.rfind(' ', cut);
    if (space != std::string::npos && space > max / 2) cut = space;
    s.resize(cut);
    while (!s.empty() && (s.back() == ' ' || s.back() == ',' || s.back() == ';' || s.back() == ':')) s.pop_back();
    return s + "…";
}

const Json* get(const Json* o, const char* k) {
    return o && o->type == Json::Obj ? dd::core::json_get(*o, k) : nullptr;
}

std::string str_of(const Json* v) {
    return v && v->type == Json::Str ? v->str : "";
}

std::vector<std::string> required_of(const Json* params) {
    std::vector<std::string> out;
    if (const Json* r = get(params, "required"); r && r->type == Json::Arr)
        for (const auto& x : r->arr)
            if (x && x->type == Json::Str) out.push_back(x->str);
    return out;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v)
        if (x == s) return true;
    return false;
}

std::string enum_values(const Json* p, const char* sep, std::size_t max_n) {
    const Json* en = get(p, "enum");
    if (!en || en->type != Json::Arr || en->arr.empty()) return "";
    std::string o;
    std::size_t n = 0;
    for (const auto& e : en->arr) {
        if (!e) continue;
        if (n == max_n) return o + sep + "…";
        o += (o.empty() ? "" : sep) + (e->type == Json::Str ? e->str : dd::core::json_dump(*e));
        ++n;
    }
    return o;
}

// What a flag takes, as typed on the slash line ("" for a boolean: the flag alone sets it).
std::string placeholder(const Json* p) {
    if (std::string ev = enum_values(p, "|", 4); !ev.empty()) return ev;
    const std::string t = str_of(get(p, "type"));
    if (t == "integer" || t == "number") return "N";
    if (t == "boolean") return "";
    if (t == "array") return "a,b";
    if (t == "object") return "'{…}'";
    return "TEXT";
}

std::string type_word(const Json* p) {
    if (std::string ev = enum_values(p, " · ", 8); !ev.empty()) return ev;
    const std::string t = str_of(get(p, "type"));
    if (t == "array") {
        const std::string it = str_of(get(get(p, "items"), "type"));
        return it.empty() ? "list" : "list of " + it;
    }
    return t.empty() ? "value" : t;
}

// The property the slash line's words fill: the same rule slash_args applies.
std::string primary_of(const Json* params, const Json* props) {
    for (const auto& r : required_of(params))
        if (str_of(get(get(props, r.c_str()), "type")) == "string") return r;
    return "";
}

}  // namespace

std::string tool_purpose(const std::string& description, std::size_t max) {
    const std::string d = squash(description);
    std::string out;
    std::size_t i = 0;
    while (i < d.size()) {
        std::size_t e = i;
        for (; e < d.size(); ++e)   // a sentence ends at . ! ? followed by a space or the end
            if ((d[e] == '.' || d[e] == '!' || d[e] == '?') && (e + 1 == d.size() || d[e + 1] == ' ')) break;
        const std::size_t end = e < d.size() ? e + 1 : d.size();
        out += (out.empty() ? "" : " ") + d.substr(i, end - i);
        i = end;
        while (i < d.size() && d[i] == ' ') ++i;
        if (out.size() >= 40) break;   // a first sentence that says little takes the next
    }
    return clip(out, max);
}

std::string usage_synopsis(const Json* params, bool full) {
    const Json* props = get(params, "properties");
    if (!props || props->type != Json::Obj) return "";
    const std::vector<std::string> req = required_of(params);
    const std::string prim = primary_of(params, props);
    std::string o;
    const auto add = [&o](const std::string& part) { o += (o.empty() ? "" : " ") + part; };
    for (const auto& r : req) {
        if (r == prim) {
            add("<" + r + ">");
            continue;
        }
        const std::string ph = placeholder(get(props, r.c_str()));
        add("--" + r + (ph.empty() ? "" : " " + ph));
    }
    bool optional = false;
    for (const auto& [k, v] : props->obj) {
        if (contains(req, k)) continue;
        optional = true;
        if (!full) break;
        const std::string ph = placeholder(v.get());
        add("[--" + k + (ph.empty() ? "" : " " + ph) + "]");
    }
    if (optional && !full) add("[options]");
    return o;
}

std::string tools_index_md(const std::string& title, const std::vector<ToolDoc>& tools, const std::string& slash) {
    std::string md = "### " + title + "\n\n";
    if (tools.empty()) return md + "No tools.\n";
    md += slash.empty() ? "| tool | what it is for |\n|---|---|\n" : "| tool | what it is for | usage |\n|---|---|---|\n";
    for (const ToolDoc& t : tools) {
        const std::string purpose = t.description.empty() ? "(the server gives no description)" : tool_purpose(t.description);
        md += "| `" + t.name + "` | " + cell(purpose);
        if (!slash.empty()) {
            const std::string syn = usage_synopsis(t.params, false);
            md += " | `" + slash + " " + t.name + (syn.empty() ? "" : " " + syn) + "`";
        }
        md += " |\n";
    }
    return md;
}

std::string tool_detail_md(const ToolDoc& t, const std::string& slash) {
    std::string md = "### `" + t.name + "` · " + (t.toolset.empty() ? "built-in" : t.toolset) + "\n\n";
    md += t.description.empty() ? "The server gives no description.\n" : t.description + "\n";
    if (!slash.empty()) {
        const std::string syn = usage_synopsis(t.params, true);
        md += "\n**Usage** `" + slash + " " + t.name + (syn.empty() ? "" : " " + syn) + "`\n";
    }
    if (!t.wire.empty() && t.wire != t.name) md += "\n**The model calls it** `" + t.wire + "`\n";
    const Json* props = get(t.params, "properties");
    if (!props || props->type != Json::Obj || props->obj.empty()) return md + "\nNo parameters.\n";
    const std::vector<std::string> req = required_of(t.params);
    md += "\n| parameter | type | | default | what it does |\n|---|---|---|---|---|\n";
    for (const auto& [k, v] : props->obj) {
        const Json* def = get(v.get(), "default");
        std::string d;
        if (def) d = "`" + (def->type == Json::Str ? def->str : dd::core::json_dump(*def)) + "`";
        md += "| `" + k + "` | " + cell(type_word(v.get())) + " | " + (contains(req, k) ? "**required**" : "") + " | " + d +
              " | " + cell(str_of(get(v.get(), "description"))) + " |\n";
    }
    return md;
}

}  // namespace tools
}  // namespace dd
