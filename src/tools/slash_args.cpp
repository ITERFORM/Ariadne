#include "dd/tools/slash_args.hpp"

#include <cctype>
#include <cstdlib>
#include <vector>

namespace dd {
namespace tools {
namespace {

using dd::core::Json;

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

// Shell-ish words: a quote that opens a word groups it ("XRP ledger"); an apostrophe inside a word is a letter.
std::vector<std::string> words_of(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool quoted = false;   // this word was opened by a quote, so it counts even when empty
    char q = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (q) {
            if (c == q) q = 0;
            else if (c == '\\' && q == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += c;
            continue;
        }
        if ((c == '"' || c == '\'') && cur.empty() && !quoted) {
            q = c;
            quoted = true;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (quoted || !cur.empty()) out.push_back(cur);
            cur.clear();
            quoted = false;
            continue;
        }
        cur += c;
    }
    if (quoted || !cur.empty()) out.push_back(cur);
    return out;
}

const Json* prop_of(const Json* props, const std::string& name) {
    return props && props->type == Json::Obj ? dd::core::json_get(*props, name) : nullptr;
}

std::string type_of(const Json* p) {
    const Json* t = p && p->type == Json::Obj ? dd::core::json_get(*p, "type") : nullptr;
    return t && t->type == Json::Str ? t->str : "";
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

int bool_word(const std::string& s) {   // 1 true, 0 false, -1 not a boolean word
    const std::string l = lower(s);
    if (l == "true" || l == "yes" || l == "on" || l == "1") return 1;
    if (l == "false" || l == "no" || l == "off" || l == "0") return 0;
    return -1;
}

// `raw` as the property's declared type; false when it is not one (the words then stay in the text).
bool set_typed(Json& out, const std::string& key, const std::string& type, const std::string& raw) {
    if (type == "integer" || type == "number") {
        if (raw.empty()) return false;
        char* end = nullptr;
        const double v = std::strtod(raw.c_str(), &end);
        if (!end || *end) return false;
        if (type == "integer" && static_cast<double>(static_cast<long long>(v)) != v) return false;
        dd::core::obj_set(out, key, dd::core::json_num(v));
        return true;
    }
    if (type == "boolean") {
        const int b = bool_word(raw);
        if (b < 0) return false;
        dd::core::obj_set(out, key, dd::core::json_bool(b == 1));
        return true;
    }
    if (type == "object") {   // --args '{"q": "x"}': JSON, typed as such
        if (raw.empty() || raw.front() != '{') return false;
        bool repaired = false;
        Json o = dd::core::json_parse_args(raw, repaired);
        if (o.type != Json::Obj) return false;
        dd::core::obj_set(out, key, o);
        return true;
    }
    if (type == "array") {   // a,b,c
        Json a = dd::core::json_arr();
        size_t i = 0;
        while (i <= raw.size()) {
            size_t e = raw.find(',', i);
            if (e == std::string::npos) e = raw.size();
            if (const std::string part = trim(raw.substr(i, e - i)); !part.empty()) dd::core::arr_push(a, dd::core::json_str(part));
            i = e + 1;
        }
        if (a.arr.empty()) return false;
        dd::core::obj_set(out, key, a);
        return true;
    }
    dd::core::obj_set(out, key, dd::core::json_str(raw));
    return true;
}

// Where the words go: the first required string property, else the usual one-arg names, else any string.
std::string primary_of(const Json& schema, const Json* props) {
    if (const Json* req = dd::core::json_get(schema, "required"); req && req->type == Json::Arr)
        for (const auto& r : req->arr)
            if (r && r->type == Json::Str && type_of(prop_of(props, r->str)) == "string") return r->str;
    for (const char* k : {"query", "topic", "q", "text", "prompt"})
        if (prop_of(props, k)) return k;
    for (const auto& [k, v] : props->obj)
        if (type_of(v.get()) == "string") return k;
    return "query";
}

}  // namespace

Json slash_args(const std::string& text, const Json* schema) {
    const std::string t = trim(text);
    if (t.empty()) return dd::core::json_obj();
    if (t.front() == '{') {   // typed as JSON: passed through as typed
        bool repaired = false;
        Json j = dd::core::json_parse_args(t, repaired);
        if (j.type == Json::Obj) return j;
    }
    const Json* props = schema && schema->type == Json::Obj ? dd::core::json_get(*schema, "properties") : nullptr;
    if (!props || props->type != Json::Obj || props->obj.empty())
        return dd::core::json_obj({{"query", std::make_shared<Json>(dd::core::json_str(t))}});

    Json out = dd::core::json_obj();
    std::vector<std::string> loose;
    const std::vector<std::string> w = words_of(t);
    for (size_t i = 0; i < w.size(); ++i) {
        const std::string& tok = w[i];
        const bool dbl = tok.size() > 2 && tok.compare(0, 2, "--") == 0;
        const bool sgl = !dbl && tok.size() == 2 && tok[0] == '-' && std::isalpha(static_cast<unsigned char>(tok[1]));
        if (!dbl && !sgl) {
            loose.push_back(tok);
            continue;
        }
        std::string name = tok.substr(dbl ? 2 : 1), val;
        bool has_val = false;
        if (const size_t eq = name.find('='); eq != std::string::npos) {
            val = name.substr(eq + 1);
            name.resize(eq);
            has_val = true;
        }
        std::string key = name;
        for (char& c : key)
            if (c == '-') c = '_';
        if (const Json* p = prop_of(props, key)) {
            const std::string ty = type_of(p);
            if (ty == "boolean") {
                if (has_val) {
                    if (set_typed(out, key, ty, val)) continue;
                } else if (i + 1 < w.size() && bool_word(w[i + 1]) >= 0) {
                    set_typed(out, key, ty, w[++i]);
                    continue;
                } else {
                    dd::core::obj_set(out, key, dd::core::json_bool(true));
                    continue;
                }
            } else {
                const bool took = !has_val && i + 1 < w.size();
                if (took) val = w[i + 1];
                if ((has_val || took) && set_typed(out, key, ty, val)) {
                    if (took) ++i;
                    continue;
                }
            }
        }
        if (key.rfind("no_", 0) == 0)   // --no-name clears a boolean
            if (const Json* bp = prop_of(props, key.substr(3)); bp && type_of(bp) == "boolean") {
                dd::core::obj_set(out, key.substr(3), dd::core::json_bool(false));
                continue;
            }
        if (!has_val) {   // --deep: the one property whose enum lists it
            std::string owner;
            int owners = 0;
            for (const auto& [k, v] : props->obj)
                if (const Json* en = v && v->type == Json::Obj ? dd::core::json_get(*v, "enum") : nullptr; en && en->type == Json::Arr)
                    for (const auto& e : en->arr)
                        if (e && e->type == Json::Str && e->str == name) {
                            owner = k;
                            ++owners;
                        }
            if (owners == 1) {
                dd::core::obj_set(out, owner, dd::core::json_str(name));
                continue;
            }
        }
        loose.push_back(tok);   // undeclared: it stays in the text
    }
    if (!loose.empty()) {
        std::string words;
        for (const std::string& s : loose) words += (words.empty() ? "" : " ") + s;
        const std::string key = primary_of(*schema, props);
        if (const Json* have = dd::core::json_get(out, key); have && have->type == Json::Str && !have->str.empty())
            words = have->str + " " + words;
        dd::core::obj_set(out, key, dd::core::json_str(words));
    }
    return out;
}

}  // namespace tools
}  // namespace dd
