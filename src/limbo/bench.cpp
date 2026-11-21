// src/limbo/bench.cpp — `ariadne limbo bench`. py: daedalus_cli/stack_bench.py.
#include "dd/limbo/bench.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <regex>
#include <sstream>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>

#include "dd/limbo/afe.hpp"
#include "dd/net/http.hpp"

namespace fs = std::filesystem;
using dd::core::Json;

namespace dd {
namespace limbo {
namespace bench {

namespace {

constexpr int kChatMaxTokens = 100;
constexpr int kLongMaxTokens = 24;
constexpr int kAgentMaxTokens = 64;
constexpr int kAgentTurns = 5;
constexpr int kAgentWordsPerTurn = 1200;
constexpr long kChatTimeoutMs = 600000;
constexpr long kLongTimeoutMs = 1800000;
// Measured on this vocabulary with the Qwen3.6 tokenizer: 1.06 tokens per word. A ratio, not a
// promise: every run reports the prompt it actually saw.
constexpr double kTokensPerWord = 1.06;

const char* const kQuestions[3] = {
    "Write a Python function that merges two sorted lists into one sorted list, with a docstring and two tests.",
    "Explain in two paragraphs how a refrigerator moves heat from inside to outside.",
    "List ten practical tips for learning a new language as an adult, one sentence each.",
};

const char* const kAgentSystem =
    "You are an autonomous coding agent working in the user's repository. You can read files, search the code, "
    "edit files and run commands with the tools below. Work step by step: read the code you need before changing "
    "it, make small and correct edits, keep the existing style, and explain briefly what you did. Never invent "
    "file contents: read them. When the task is done, give a short summary of the changes and how to test them.";

const char* const kAgentTools = R"([
{"type":"function","function":{"name":"read_file","description":"Read a file of the repository (UTF-8 text).","parameters":{"type":"object","properties":{"path":{"type":"string","description":"path from the repository root"}},"required":["path"]}}},
{"type":"function","function":{"name":"grep","description":"Search the repository for a regular expression; returns matching lines.","parameters":{"type":"object","properties":{"pattern":{"type":"string"},"path":{"type":"string"}},"required":["pattern"]}}},
{"type":"function","function":{"name":"edit_file","description":"Replace an exact string in a file with a new one.","parameters":{"type":"object","properties":{"path":{"type":"string"},"old":{"type":"string"},"new":{"type":"string"}},"required":["path","old","new"]}}},
{"type":"function","function":{"name":"run","description":"Run a shell command in the repository and return its output.","parameters":{"type":"object","properties":{"command":{"type":"string"}},"required":["command"]}}}
])";

const char* const kAgentTurnsText[kAgentTurns] = {
    "Add a --max-queue N option to the HTTP server that rejects new requests with 503 once N are waiting.",
    "Now document that option in the README's settings section.",
    "Add a test for the rejection path, including the boundary at exactly N.",
    "The queue depth is currently counted per connection; fix it to count per request.",
    "Summarise the change and say how to verify it.",
};

// A canned earlier reply grows the conversation like a session and keeps sizes identical.
const char* const kAgentReply =
    "I read the option parser and the request path first. The new flag is parsed next to the existing ones and the "
    "counter lives with the other per-server state, so no global is needed. I kept the existing style, added the "
    "boundary case at exactly N, and updated the settings section of the README with the default and an example. "
    "To verify: start the server, send N+1 concurrent requests, and expect 503 on the last one only.";

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

std::optional<double> num_of(const Json& o, const char* key) {
    if (o.type != Json::Obj) return std::nullopt;
    const Json* v = core::json_get(o, key);
    if (!v || v->type != Json::Num) return std::nullopt;
    return v->num;
}

Json jnum(std::optional<double> v) { return v ? core::json_num(*v) : core::json_null(); }

std::string fmt_num(std::optional<double> v, int digits) {
    if (!v) return "None";
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", digits, *v);
    return b;
}

std::vector<std::string> split_words(const char* s) {
    std::istringstream in(s);
    std::vector<std::string> w;
    std::string x;
    while (in >> x) w.push_back(x);
    return w;
}

}  // namespace

// ---- prompts ---------------------------------------------------------------------------

std::string long_prompt(int words, unsigned salt) {
    static const auto vocab = split_words("river stone market lantern copper winter garden signal harbor needle orbit "
                                          "velvet canyon ember ledger quartz meadow anchor violet thunder");
    std::mt19937 rng(salt);
    std::uniform_int_distribution<size_t> pick(0, vocab.size() - 1);
    std::string body;
    for (int i = 0; i < words; ++i) body += (i ? " " : "") + vocab[pick(rng)];
    return "Run " + std::to_string(salt % 1000) + ": here is a list of words:\n" + body +
           "\n\nSummarize what kinds of words appear in the list, in five sentences.";
}

std::string file_chunk(int turn, int words) {
    static const auto vocab = split_words("def return class import async await struct pointer buffer cache tensor kernel "
                                          "index stride token prompt matrix vector socket thread module option config "
                                          "request response error timeout");
    std::mt19937 rng(1000u + static_cast<unsigned>(turn));
    std::uniform_int_distribution<size_t> pick(0, vocab.size() - 1);
    std::uniform_int_distribution<int> n1000(0, 999);
    std::string out;
    const int lines = std::max(1, words / 12);
    for (int i = 0; i < lines; ++i) {
        char idx[16];
        std::snprintf(idx, sizeof idx, "%4d", i);
        out += std::string(idx) + "  " + vocab[pick(rng)] + " " + vocab[pick(rng)] + "_" + vocab[pick(rng)] + " = " +
               vocab[pick(rng)] + "(" + std::to_string(n1000(rng)) + ")" + (i + 1 < lines ? "\n" : "");
    }
    return out;
}

// ---- streaming chat --------------------------------------------------------------------

bool sse_event(const std::string& payload, double t, ChatResult& r) {
    if (payload == "[DONE]") return false;
    Json j;
    try { j = core::json_parse(payload); } catch (...) { return true; }
    if (j.type != Json::Obj) return true;
    if (const Json* e = core::json_get(j, "error"); e && e->type != Json::Null)
        r.err = core::json_dump(*e).substr(0, 300);
    if (const Json* ch = core::json_get(j, "choices"); ch && ch->type == Json::Arr) {
        for (const auto& c : ch->arr) {
            const Json* d = c ? core::json_get(*c, "delta") : nullptr;
            if (!d || d->type != Json::Obj) continue;
            std::string content, piece;
            if (const Json* x = core::json_get(*d, "content"); x && x->type == Json::Str) content = x->str;
            piece = content;
            if (const Json* x = core::json_get(*d, "reasoning_content"); x && x->type == Json::Str) piece += x->str;
            // a tool call is produced output too: the agent section sends tools
            if (piece.empty()) {
                if (const Json* tc = core::json_get(*d, "tool_calls"); tc && tc->type == Json::Arr && !tc->arr.empty()) {
                    for (const auto& call : tc->arr) {
                        const Json* fn = call ? core::json_get(*call, "function") : nullptr;
                        const Json* nm = fn ? core::json_get(*fn, "name") : nullptr;
                        if (nm && nm->type == Json::Str) piece += nm->str;
                    }
                    if (piece.empty()) piece = "?";
                }
            }
            if (!piece.empty()) {
                if (r.first < 0) r.first = t;
                ++r.pieces;
                r.text += content;
            }
        }
    }
    if (const Json* u = core::json_get(j, "usage"); u && u->type == Json::Obj) r.usage = *u;
    if (const Json* tm = core::json_get(j, "timings"); tm && tm->type == Json::Obj) r.timings = *tm;
    return true;
}

namespace {

ChatResult chat(const std::string& base, const std::string& key, const std::string& model, const Json& messages,
                int max_tokens, bool think, const Json* tools, long timeout_ms) {
    Json body = core::json_obj();
    core::obj_set(body, "model", core::json_str(model));
    core::obj_set(body, "stream", core::json_bool(true));
    core::obj_set(body, "max_tokens", core::json_num(max_tokens));
    core::obj_set(body, "temperature", core::json_num(0));
    core::obj_set(body, "messages", messages);
    if (tools) core::obj_set(body, "tools", *tools);
    if (!think) {
        core::obj_set(body, "reasoning_effort", core::json_str("none"));
        Json kw = core::json_obj();
        core::obj_set(kw, "enable_thinking", core::json_bool(false));
        core::obj_set(body, "chat_template_kwargs", kw);
    }
    std::vector<std::string> headers;
    if (!key.empty()) headers.push_back("Authorization: Bearer " + key);
    ChatResult r;
    const double t0 = now_s();
    std::string buf;
    bool done = false;
    const net::Response resp = net::post_stream(base + "/v1/chat/completions", core::json_dump(body), timeout_ms, nullptr,
        [&](std::string_view chunk) {
            buf.append(chunk.data(), chunk.size());
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (line.rfind("data:", 0) != 0) continue;
                std::string payload = line.substr(5);
                payload.erase(0, payload.find_first_not_of(' '));
                if (!sse_event(payload, now_s() - t0, r)) { done = true; return false; }
            }
            return true;
        }, headers);
    if (!done && !resp.error.empty()) r.err = resp.error.substr(0, 300);
    else if (resp.status >= 400) r.err = "HTTP " + std::to_string(resp.status);
    r.total = now_s() - t0;
    return r;
}

Json messages_of(std::initializer_list<std::pair<const char*, std::string>> m) {
    Json a = core::json_arr();
    for (const auto& [role, content] : m) {
        Json one = core::json_obj();
        core::obj_set(one, "role", core::json_str(role));
        core::obj_set(one, "content", core::json_str(content));
        core::arr_push(a, one);
    }
    return a;
}

}  // namespace

Digest digest(const ChatResult& r) {
    Digest d;
    const Json& u = r.usage;
    const Json& t = r.timings;
    std::optional<double> gen = num_of(u, "completion_tokens");
    if (!gen || *gen == 0) gen = static_cast<double>(r.pieces);
    std::optional<double> prompt = num_of(u, "prompt_tokens");
    std::optional<double> cached;
    if (u.type == Json::Obj) {
        if (const Json* det = core::json_get(u, "prompt_tokens_details")) cached = num_of(*det, "cached_tokens");
    }
    if (!cached) cached = num_of(t, "cache_n");
    std::optional<double> read = num_of(t, "prompt_n");
    // llama-server sends usage: null on a stream; prompt_n + cache_n is what the template produced
    if (!prompt && read) prompt = *read + cached.value_or(0);
    if (!read && prompt && cached) read = *prompt - *cached;
    std::optional<double> dec = num_of(t, "predicted_per_second");
    std::string src = "engine";
    if (!dec && r.first >= 0 && gen && *gen > 1 && r.total > r.first) {
        dec = std::round((*gen - 1) / (r.total - r.first) * 10) / 10;
        src = "client";
    }
    d.ok = r.err.empty() && r.first >= 0;
    d.err = r.err;
    if (r.first >= 0) d.ttft_s = std::round(r.first * 1000) / 1000;
    d.total_s = std::round(r.total * 1000) / 1000;
    d.prompt_tokens = prompt;
    d.reused_tokens = cached;
    d.read_tokens = read;
    d.generated_tokens = gen;
    d.prompt_tps = num_of(t, "prompt_per_second");
    d.decode_tps = dec;
    d.decode_source = dec ? src : "";
    d.prompt_ms = num_of(t, "prompt_ms");
    d.decode_ms = num_of(t, "predicted_ms");
    d.drafts_offered = num_of(t, "draft_n");
    d.drafts_accepted = num_of(t, "draft_n_accepted");
    return d;
}

// ---- statistics ------------------------------------------------------------------------

std::optional<Stat> stat(const std::vector<std::optional<double>>& values) {
    std::vector<double> v;
    for (const auto& x : values) if (x) v.push_back(*x);
    if (v.empty()) return std::nullopt;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    const double med = n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2;
    auto r3 = [](double x) { return std::round(x * 1000) / 1000; };
    return Stat{r3(med), r3(v.front()), r3(v.back()), static_cast<int>(n)};
}

std::optional<double> pctl(std::vector<std::optional<double>> values, double p) {
    std::vector<double> v;
    for (const auto& x : values) if (x) v.push_back(*x);
    if (v.empty()) return std::nullopt;
    std::sort(v.begin(), v.end());
    const long k = std::max(0L, std::min(static_cast<long>(v.size()) - 1,
                                         std::lround(p / 100.0 * static_cast<double>(v.size() - 1))));
    return v[static_cast<size_t>(k)];
}

std::string redact(const std::string& text, const std::string& home, const std::string& user, const std::string& host) {
    std::string s = text;
    if (!home.empty()) {
        for (size_t p = s.find(home); p != std::string::npos; p = s.find(home, p + 1)) s.replace(p, home.size(), "~");
    }
    auto word = [&](const std::string& value, const char* repl) {
        if (value.size() <= 2) return;
        static const std::regex special(R"([.^$|()\[\]{}*+?\\])");
        const std::string esc = std::regex_replace(value, special, R"(\$&)");
        s = std::regex_replace(s, std::regex("\\b" + esc + "\\b"), repl);
    };
    word(user, "<user>");
    word(host, "<host>");
    return s;
}

std::string slug(const std::string& text, size_t limit) {
    std::string out;
    bool dash = false;
    for (char c : text) {
        const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (std::isalnum(static_cast<unsigned char>(l))) { out += l; dash = false; }
        else if (!dash && !out.empty()) { out += '-'; dash = true; }
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    if (out.size() > limit) out.resize(limit);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out;
}

// ---- the suite -------------------------------------------------------------------------

namespace {

struct Row {
    std::vector<Digest> reqs;
};

Json digest_json(const Digest& d) {
    Json o = core::json_obj();
    core::obj_set(o, "ok", core::json_bool(d.ok));
    core::obj_set(o, "err", d.err.empty() ? core::json_null() : core::json_str(d.err));
    core::obj_set(o, "ttft_s", jnum(d.ttft_s));
    core::obj_set(o, "total_s", jnum(d.total_s));
    core::obj_set(o, "prompt_tokens", jnum(d.prompt_tokens));
    core::obj_set(o, "reused_tokens", jnum(d.reused_tokens));
    core::obj_set(o, "read_tokens", jnum(d.read_tokens));
    core::obj_set(o, "generated_tokens", jnum(d.generated_tokens));
    core::obj_set(o, "prompt_tps", jnum(d.prompt_tps));
    core::obj_set(o, "decode_tps", jnum(d.decode_tps));
    core::obj_set(o, "decode_tps_source", d.decode_source.empty() ? core::json_null() : core::json_str(d.decode_source));
    core::obj_set(o, "prompt_ms", jnum(d.prompt_ms));
    core::obj_set(o, "decode_ms", jnum(d.decode_ms));
    core::obj_set(o, "drafts_offered", jnum(d.drafts_offered));
    core::obj_set(o, "drafts_accepted", jnum(d.drafts_accepted));
    return o;
}

Json stat_json(const std::optional<Stat>& s) {
    if (!s) return core::json_null();
    Json o = core::json_obj();
    core::obj_set(o, "median", core::json_num(s->median));
    core::obj_set(o, "min", core::json_num(s->min));
    core::obj_set(o, "max", core::json_num(s->max));
    core::obj_set(o, "n", core::json_num(s->n));
    return o;
}

struct Accept { double offered = 0, accepted = 0; };
std::optional<Accept> accept_rate(const std::vector<const Digest*>& rows) {
    Accept a;
    for (const Digest* d : rows) {
        if (d->drafts_offered) a.offered += *d->drafts_offered;
        if (d->drafts_accepted) a.accepted += *d->drafts_accepted;
    }
    if (a.offered <= 0) return std::nullopt;
    return a;
}

// One section's summary: medians over the runs that worked.
struct Section {
    std::vector<Digest> reqs;
    std::optional<double> reuse_pct;
    bool ran = false;
    bool ok() const { for (const auto& d : reqs) if (d.ok) return true; return false; }
    std::optional<Stat> med(std::optional<double> Digest::*f) const {
        std::vector<std::optional<double>> v;
        for (const auto& d : reqs) if (d.ok) v.push_back(d.*f);
        return stat(v);
    }
};

std::string fmt_stat(const std::optional<Stat>& s, int digits) {
    if (!s) return "--";
    char b[48];
    std::snprintf(b, sizeof b, "%.*f", digits, s->median);
    return b;
}

Json section_json(const Section& s) {
    Json o = core::json_obj();
    int ok = 0;
    for (const auto& d : s.reqs) ok += d.ok ? 1 : 0;
    core::obj_set(o, "runs", core::json_num(static_cast<double>(s.reqs.size())));
    core::obj_set(o, "ok", core::json_num(ok));
    core::obj_set(o, "prompt_tokens", stat_json(s.med(&Digest::prompt_tokens)));
    core::obj_set(o, "reused_tokens", stat_json(s.med(&Digest::reused_tokens)));
    core::obj_set(o, "read_tokens", stat_json(s.med(&Digest::read_tokens)));
    core::obj_set(o, "generated_tokens", stat_json(s.med(&Digest::generated_tokens)));
    core::obj_set(o, "prompt_tps", stat_json(s.med(&Digest::prompt_tps)));
    core::obj_set(o, "decode_tps", stat_json(s.med(&Digest::decode_tps)));
    core::obj_set(o, "ttft_s", stat_json(s.med(&Digest::ttft_s)));
    core::obj_set(o, "total_s", stat_json(s.med(&Digest::total_s)));
    if (s.reuse_pct) core::obj_set(o, "reuse_pct", core::json_num(*s.reuse_pct));
    Json reqs = core::json_arr();
    for (const auto& d : s.reqs) core::arr_push(reqs, digest_json(d));
    core::obj_set(o, "requests", reqs);
    return o;
}

class Suite {
public:
    Suite(std::string base, std::string key, std::string model, long ctx, int runs, bool think, int clients, bool want_long, bool quick)
        : base_(std::move(base)), key_(std::move(key)), model_(std::move(model)), ctx_(ctx), runs_(runs),
          think_(think), clients_(clients), long_(want_long), quick_(quick) {}

    Section chat_, k4_, k16_, k32_, agent_;
    std::vector<std::string> skipped_;
    Json clients_out_ = core::json_null();

    void run() {
        // the first decode after a load also builds the CUDA graphs: not charged to a question
        log("  warm-up (one short answer, not reported)");
        chat(base_, key_, model_, messages_of({{"user", "Say ok."}}), 4, think_, nullptr, kChatTimeoutMs);
        run_chat();
        run_long(k4_, "4K", words(4096), true);
        if (quick_) return;
        if (ctx_ >= 24576) run_long(k16_, "16K", words(16384), false);
        if (long_) {
            if (ctx_ >= 49152) run_long(k32_, "32K", words(32768), false);
            else log("  32K prompt: context is " + std::to_string(ctx_) + ", skipping");
        }
        run_agent();
        if (clients_ > 0) run_clients();
    }

private:
    void log(const std::string& s) const { std::cout << s << "\n" << std::flush; }

    Digest ask(const std::string& label, const Json& messages, int max_tokens, const Json* tools, long timeout_ms) {
        const Digest d = digest(chat(base_, key_, model_, messages, max_tokens, think_, tools, timeout_ms));
        std::string line = "    " + label + ": ttft " + fmt_num(d.ttft_s, 3) + "s  prompt " + fmt_num(d.prompt_tps, 1) +
                           " tok/s  decode " + fmt_num(d.decode_tps, 1) + " tok/s  gen " + fmt_num(d.generated_tokens, 0);
        if (d.reused_tokens && *d.reused_tokens > 0) line += "  reused " + fmt_num(d.reused_tokens, 0);
        if (!d.ok) line += "  ERR " + d.err;
        log(line);
        return d;
    }

    int words(int tokens) const {
        const long budget = ctx_ ? std::min<long>(tokens, std::max<long>(512, ctx_ - 512)) : tokens;
        return std::max(64, static_cast<int>(static_cast<double>(budget) / kTokensPerWord));
    }

    void run_chat() {
        log("  chat (three short questions, ~100 tokens out)");
        chat_.ran = true;
        for (int run = 0; run < runs_; ++run)
            for (int i = 0; i < 3; ++i)
                chat_.reqs.push_back(ask("chat r" + std::to_string(run + 1) + " q" + std::to_string(i + 1),
                                         messages_of({{"user", kQuestions[i]}}), kChatMaxTokens, nullptr, kChatTimeoutMs));
    }

    void run_long(Section& sec, const std::string& label, int w, bool first) {
        log("  " + label + " prompt (fresh every run)");
        std::vector<Digest> rows;
        for (int run = 0; run < runs_; ++run) {
            const unsigned salt = 7u + 977u * static_cast<unsigned>(run);
            const Digest d = ask(label + " r" + std::to_string(run + 1), messages_of({{"user", long_prompt(w, salt)}}),
                                 kLongMaxTokens, nullptr, kLongTimeoutMs);
            rows.push_back(d);
            const double tps = d.prompt_tps.value_or(0);
            if (!first && run == 0 && tps > 0 && tps < 150 && !long_) {
                // a long prompt at 100 tok/s is minutes a run: say so instead of calling it a benchmark
                char b[32];
                std::snprintf(b, sizeof b, "%.0f", tps);
                log(std::string("    prefill is ") + b + " tok/s -- skipping the longer prompt (rerun with --long to force it)");
                skipped_.push_back(label + ": prefill " + b + " tok/s");
                return;
            }
        }
        sec.reqs = rows;
        sec.ran = true;
    }

    void run_agent() {
        log("  agent session (5 turns, ~1200 words added per turn)");
        Json msgs = messages_of({{"system", kAgentSystem}});
        const Json tools = core::json_parse(kAgentTools);
        for (int turn = 0; turn < kAgentTurns; ++turn) {
            Json u = core::json_obj();
            core::obj_set(u, "role", core::json_str("user"));
            core::obj_set(u, "content", core::json_str(file_chunk(turn, kAgentWordsPerTurn) + "\n\n" + kAgentTurnsText[turn]));
            core::arr_push(msgs, u);
            agent_.reqs.push_back(ask("agent t" + std::to_string(turn + 1), msgs, kAgentMaxTokens, &tools, kChatTimeoutMs));
            Json a = core::json_obj();
            core::obj_set(a, "role", core::json_str("assistant"));
            core::obj_set(a, "content", core::json_str(kAgentReply));
            core::arr_push(msgs, a);
        }
        agent_.ran = true;
        double reused = 0, prompt = 0;
        for (const auto& d : agent_.reqs) {
            if (!d.prompt_tokens) continue;
            prompt += *d.prompt_tokens;
            reused += d.reused_tokens.value_or(0);
        }
        if (prompt > 0) agent_.reuse_pct = std::round(1000.0 * reused / prompt) / 10;
    }

    void run_clients() {
        log("  " + std::to_string(clients_) + " clients at once");
        // one result slot per client thread, read only after join: no lock
        std::vector<ChatResult> results(static_cast<size_t>(clients_));
        std::vector<std::thread> threads;
        const double t0 = now_s();
        for (int i = 0; i < clients_; ++i) {
            threads.emplace_back([this, i, &results] {
                results[static_cast<size_t>(i)] = chat(base_, key_, model_, messages_of({{"user", kQuestions[i % 3]}}),
                                                       kChatMaxTokens, think_, nullptr, kChatTimeoutMs);
            });
        }
        for (auto& t : threads) t.join();
        const double wall = now_s() - t0;
        double gen = 0;
        std::vector<std::optional<double>> lat, ttft;
        Json errs = core::json_arr();
        int nerr = 0;
        for (const auto& r : results) {
            const Digest d = digest(r);
            gen += d.generated_tokens.value_or(0);
            lat.push_back(d.total_s);
            ttft.push_back(d.ttft_s);
            if (!d.err.empty() && nerr++ < 3) core::arr_push(errs, core::json_str(d.err));
        }
        clients_out_ = core::json_obj();
        core::obj_set(clients_out_, "n", core::json_num(clients_));
        core::obj_set(clients_out_, "wall_s", core::json_num(std::round(wall * 1000) / 1000));
        core::obj_set(clients_out_, "generated_tokens", core::json_num(gen));
        core::obj_set(clients_out_, "total_tps", wall > 0 ? core::json_num(std::round(gen / wall * 10) / 10) : core::json_null());
        core::obj_set(clients_out_, "latency_p50_s", jnum(pctl(lat, 50)));
        core::obj_set(clients_out_, "latency_p95_s", jnum(pctl(lat, 95)));
        core::obj_set(clients_out_, "ttft_p50_s", jnum(pctl(ttft, 50)));
        core::obj_set(clients_out_, "errors", errs);
        log("    " + (wall > 0 ? fmt_num(std::round(gen / wall * 10) / 10, 1) : std::string("None")) + " tok/s total, p50 " +
            fmt_num(pctl(lat, 50), 3) + "s p95 " + fmt_num(pctl(lat, 95), 3) + "s");
    }

    std::string base_, key_, model_;
    long ctx_;
    int runs_;
    bool think_;
    int clients_;
    bool long_, quick_;
};

// Peak VRAM and server RSS while the suite runs: the other half of "does it fit".
class Sampler {
public:
    explicit Sampler(pid_t pid) : pid_(pid), smi_(!which("nvidia-smi").empty()) {}
    void start() { th_ = std::thread([this] { loop(); }); }
    void stop() { if (th_.joinable()) { stop_.store(true); th_.join(); } }
    long vram() const { return vram_.load(); }
    long rss() const { return rss_.load(); }
    long swap() const { return swap_.load(); }
    long samples() const { return samples_.load(); }

private:
    void loop() {
        while (!stop_.load()) {
            if (smi_) {
                std::string out;
                if (run_capture({"nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"}, out, 5) == 0)
                    raise(vram_, std::atol(out.c_str()));
            }
            if (pid_ > 0) {
                std::ifstream in("/proc/" + std::to_string(pid_) + "/status");
                std::string line;
                while (std::getline(in, line)) {
                    if (line.rfind("VmRSS:", 0) == 0) raise(rss_, std::atol(line.c_str() + 6) / 1024);
                    else if (line.rfind("VmSwap:", 0) == 0) raise(swap_, std::atol(line.c_str() + 7) / 1024);
                }
            }
            samples_.fetch_add(1);
            for (int i = 0; i < 10 && !stop_.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    static void raise(std::atomic<long>& peak, long v) {
        long cur = peak.load();
        while (v > cur && !peak.compare_exchange_weak(cur, v)) {}
    }
    pid_t pid_;
    bool smi_;
    std::thread th_;
    std::atomic<bool> stop_{false};
    std::atomic<long> vram_{0}, rss_{0}, swap_{0}, samples_{0};
};

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string gpu_name() {
    if (which("nvidia-smi").empty()) return "";
    std::string out;
    if (run_capture({"nvidia-smi", "--query-gpu=name", "--format=csv,noheader"}, out, 5) != 0) return "";
    out = out.substr(0, out.find('\n'));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

double ram_gib() {
    std::ifstream in("/proc/meminfo");
    std::string line;
    while (std::getline(in, line))
        if (line.rfind("MemTotal:", 0) == 0) return std::atof(line.c_str() + 9) / 1024 / 1024;
    return 0;
}

std::string iso_now() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char b[40];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S%z", &tm);
    std::string s = b;
    if (s.size() > 5) s.insert(s.size() - 2, ":");
    return s;
}

std::string today() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char b[16];
    std::strftime(b, sizeof b, "%Y-%m-%d", &tm);
    return b;
}

fs::path unique_dir(const fs::path& parent, const std::string& name) {
    std::error_code ec;
    fs::path p = parent / name;
    for (int i = 2; fs::exists(p, ec); ++i) p = parent / (name + "-" + std::to_string(i));
    return p;
}

}  // namespace

int cmd_bench(const Stack& s, const Options& o) {
    const StackConf c = s.conf(true, false);
    const bool quick = o.quick;
    const int runs = quick ? 1 : std::max(1, o.runs);
    std::string base, profile;
    int port = 0;
    bool started = false;
    if (!o.url.empty()) {
        base = o.url;
        while (!base.empty() && base.back() == '/') base.pop_back();
    } else {
        const auto [host, p] = c.host_port("main");
        port = p;
        base = "http://" + probe_addr(host) + ":" + std::to_string(port);
        profile = o.profile;
        if (profile.empty()) profile = s.running_profile();
        if (profile.empty()) profile = s.default_profile(c);
        if (!health_ok(host, port)) {
            if (o.no_start) { s.out().bad("nothing is serving " + base, "— start it, or drop --no-start"); return 1; }
            if (afe::serving(s, c)) {
                const auto [ah, ap] = afe::host_port(c);
                s.out().bad("AFE model is serving on " + ah + ":" + std::to_string(ap) + " — never two models",
                            "— stop it first: ariadne limbo afe stop");
                return 1;
            }
            const Launch l = s.main_launch(c, profile);
            if (l.model.empty()) { s.out().bad(profile + " model missing", "— ariadne limbo setup, or fix stack.conf"); return 1; }
            s.out().section("Starting " + profile + " for the bench");
            if (!s.start_one("main", l)) return 1;
            std::ofstream(s.paths().run() / "main.profile", std::ios::trunc) << profile;
            if (!s.wait_ready("main", host, port, 900)) { s.out().bad("the server did not become ready in 15 minutes"); return 1; }
            started = true;
            s.out().ok(profile + " ready on " + base);
        }
    }

    auto get_json = [&](const std::string& path) -> Json {
        const net::Response r = net::get(base + path, 20000);
        if (!r.ok()) return core::json_null();
        try { return core::json_parse(r.body); } catch (...) { return core::json_null(); }
    };
    const Json props = get_json("/props");
    const Json models = get_json("/v1/models");
    std::string alias;
    if (const Json* data = models.type == Json::Obj ? core::json_get(models, "data") : nullptr;
        data && data->type == Json::Arr && !data->arr.empty() && data->arr[0]) {
        if (const Json* id = core::json_get(*data->arr[0], "id"); id && id->type == Json::Str) alias = id->str;
    }
    long n_ctx = static_cast<long>(num_of(props, "n_ctx").value_or(0));
    if (!n_ctx && props.type == Json::Obj) {
        if (const Json* g = core::json_get(props, "default_generation_settings")) n_ctx = static_cast<long>(num_of(*g, "n_ctx").value_or(0));
    }
    if (!n_ctx) {
        const Json slots = get_json("/slots");   // not every engine answers /props
        if (slots.type == Json::Arr && !slots.arr.empty() && slots.arr[0]) n_ctx = static_cast<long>(num_of(*slots.arr[0], "n_ctx").value_or(0));
    }
    const long n_slots = static_cast<long>(num_of(props, "total_slots").value_or(1));
    std::string version;
    if (props.type == Json::Obj) {
        if (const Json* b = core::json_get(props, "build_info"); b && b->type == Json::Str) version = b->str;
        else if (b && b->type == Json::Obj) {
            if (const Json* v = core::json_get(*b, "build_number")) version = v->type == Json::Str ? v->str : core::json_dump(*v);
            else if (const Json* v2 = core::json_get(*b, "version")) version = v2->type == Json::Str ? v2->str : core::json_dump(*v2);
        }
    }
    std::string model_path;
    if (props.type == Json::Obj) {
        if (const Json* mp = core::json_get(props, "model_path"); mp && mp->type == Json::Str) model_path = mp->str;
    }
    std::string label = !model_path.empty() ? fs::path(model_path).filename().string() : (!alias.empty() ? alias : "unknown");
    if (!o.url.empty() && props.type == Json::Null && models.type == Json::Null) {
        s.out().bad("nothing answers at " + base, "— is that the right URL? (/props and /v1/models both failed)");
        if (started) s.stop_one("main");
        return 1;
    }

    s.out().section("Bench");
    s.out().dim("  " + base + "  model " + label + "  ctx " + std::to_string(n_ctx) + "  slots " + std::to_string(n_slots) + "  "
                + (version.empty() ? "engine unknown" : "llama.cpp " + version));
    Sampler sampler(o.url.empty() ? s.pid_of("main") : 0);
    sampler.start();
    const double t0 = now_s();
    const char* key_env = std::getenv("ARIADNE_BENCH_API_KEY");
    if (!key_env) key_env = std::getenv("DAEDALUS_API_KEY");
    Suite suite(base, key_env ? key_env : "", alias.empty() ? "ariadne" : alias, n_ctx, runs, o.think, o.clients, o.long_, quick);
    suite.run();
    sampler.stop();

    // ---- report ----
    const char* home_env = std::getenv("HOME");
    const char* user_env = std::getenv("USER");
    utsname un{};
    ::uname(&un);
    const std::string home = home_env ? home_env : "", user = user_env ? user_env : "", host = un.nodename;
    auto red = [&](const std::string& t) { return redact(t, home, user, host); };
    const std::string gpu = gpu_name();
    const double ram = ram_gib();
    char machine[160];
    std::snprintf(machine, sizeof machine, ram > 0 ? "%s %s, %u cpus, %.0f GiB RAM" : "%s %s, %u cpus", un.sysname, un.machine,
                  std::thread::hardware_concurrency(), ram);
    Out quiet;
    quiet.quiet = true;
    const Stack qs(s.paths(), quiet);
    const std::vector<std::string> launch = (o.url.empty() && !profile.empty()) ? qs.main_launch(c, profile).argv : std::vector<std::string>{};

    const std::pair<const char*, const Section*> rows[] = {{"chat", &suite.chat_}, {"4K prompt", &suite.k4_}, {"16K prompt", &suite.k16_},
                                                           {"32K prompt", &suite.k32_}, {"agent", &suite.agent_}};
    std::vector<const Digest*> all;
    for (const auto& [_, sec] : rows) for (const auto& d : sec->reqs) all.push_back(&d);
    const auto acc = accept_rate(all);

    std::ostringstream md;
    md << "# " << label << " on " << (gpu.empty() ? "unknown GPU" : gpu) << " -- `ariadne limbo bench`\n\n";
    md << "- **" << iso_now() << "**, profile **" << (profile.empty() ? "not started by ariadne" : profile) << "**, port **" << port << "**\n";
    md << "- engine: " << (version.empty() ? "engine unknown" : "llama.cpp " + version) << "\n";
    md << "- context: " << n_ctx << " tokens, " << n_slots << " slot(s), suite thinking " << (o.think ? "on" : "off")
       << ", medians of " << runs << "\n";
    if (!launch.empty()) {
        std::string joined;
        for (const auto& a : launch) joined += (joined.empty() ? "" : " ") + a;
        md << "\n```\n" << joined << "\n```\n";
    }
    md << "\n| test | prompt tok | reused | gen | prompt tok/s | decode tok/s | TTFT s |\n|---|---|---|---|---|---|---|\n";
    for (const auto& [name, sec] : rows) {
        if (!sec->ran || !sec->ok()) {
            md << "| " << name << " | -- | -- | -- | -- | -- | " << (sec->ran ? "skipped" : "not run") << " |\n";
            continue;
        }
        md << "| " << name << " | " << fmt_stat(sec->med(&Digest::prompt_tokens), 0) << " | " << fmt_stat(sec->med(&Digest::reused_tokens), 0)
           << " | " << fmt_stat(sec->med(&Digest::generated_tokens), 0) << " | " << fmt_stat(sec->med(&Digest::prompt_tps), 1) << " | "
           << fmt_stat(sec->med(&Digest::decode_tps), 1) << " | " << fmt_stat(sec->med(&Digest::ttft_s), 2) << " |\n";
    }
    if (suite.agent_.reuse_pct) md << "\nConversation cache reuse over the agent session: " << fmt_num(suite.agent_.reuse_pct, 1) << "%.\n";
    md << "\n## Speculative decoding\n";
    if (acc) {
        char b[160];
        std::snprintf(b, sizeof b, "Draft tokens accepted: **%.0f/%.0f (%.1f%%)**. A rate near zero means the draft head is not "
                                   "earning its keep on this model.\n", acc->accepted, acc->offered, 100.0 * acc->accepted / acc->offered);
        md << b;
    } else {
        md << "Not reported by this engine (no `draft_n` in the timings).\n";
    }
    md << "\n## Resources\n\n";
    if (sampler.vram() > 0) md << "- VRAM peak: **" << sampler.vram() << " MiB**\n";
    if (sampler.rss() > 0) md << "- server RSS peak: **" << sampler.rss() << " MiB**" << (sampler.swap() > 0 ? ", swap " + std::to_string(sampler.swap()) + " MiB" : "") << "\n";
    md << "- host: " << machine << "\n";
    if (!suite.skipped_.empty()) {
        std::string sk;
        for (const auto& x : suite.skipped_) sk += (sk.empty() ? "" : "; ") + x;
        md << "\nSkipped: " << sk << ".\n";
    }
    if (suite.clients_out_.type == Json::Obj) {
        const Json& cl = suite.clients_out_;
        md << "\n## " << o.clients << " clients at once\n- total **" << core::json_dump(*core::json_get(cl, "total_tps")) << " tok/s** over "
           << core::json_dump(*core::json_get(cl, "wall_s")) << "s\n- latency p50 " << core::json_dump(*core::json_get(cl, "latency_p50_s"))
           << "s, p95 " << core::json_dump(*core::json_get(cl, "latency_p95_s")) << "s, first token p50 "
           << core::json_dump(*core::json_get(cl, "ttft_p50_s")) << "s\n- " << core::json_get(cl, "errors")->arr.size() << " error(s)\n";
    }

    Json out = core::json_obj();
    core::obj_set(out, "schema", core::json_num(1));
    core::obj_set(out, "started_at", core::json_str(iso_now()));
    core::obj_set(out, "seconds", core::json_num(std::round((now_s() - t0) * 10) / 10));
    core::obj_set(out, "runs", core::json_num(runs));
    core::obj_set(out, "think", core::json_bool(o.think));
    Json ident = core::json_obj();
    core::obj_set(ident, "base", core::json_str(base));
    core::obj_set(ident, "port", core::json_num(port));
    core::obj_set(ident, "profile", core::json_str(profile));
    core::obj_set(ident, "model", core::json_str(label));
    core::obj_set(ident, "model_path", model_path.empty() ? core::json_null() : core::json_str(model_path));
    core::obj_set(ident, "alias", core::json_str(alias));
    core::obj_set(ident, "n_ctx", core::json_num(static_cast<double>(n_ctx)));
    core::obj_set(ident, "n_slots", core::json_num(static_cast<double>(n_slots)));
    core::obj_set(ident, "engine", core::json_str(version.empty() ? "engine unknown" : "llama.cpp " + version));
    core::obj_set(ident, "gpu", core::json_str(gpu));
    core::obj_set(out, "identity", ident);
    core::obj_set(out, "machine", core::json_str(machine));
    Json res = core::json_obj();
    core::obj_set(res, "vram_peak_mib", sampler.vram() ? core::json_num(sampler.vram()) : core::json_null());
    core::obj_set(res, "server_rss_peak_mib", sampler.rss() ? core::json_num(sampler.rss()) : core::json_null());
    core::obj_set(res, "server_swap_peak_mib", sampler.swap() ? core::json_num(sampler.swap()) : core::json_null());
    core::obj_set(res, "samples", core::json_num(sampler.samples()));
    core::obj_set(out, "resources", res);
    if (suite.chat_.ran) core::obj_set(out, "chat", section_json(suite.chat_));
    if (suite.k4_.ran) core::obj_set(out, "4K", section_json(suite.k4_));
    if (suite.k16_.ran) core::obj_set(out, "16K", section_json(suite.k16_));
    if (suite.k32_.ran) core::obj_set(out, "32K", section_json(suite.k32_));
    if (suite.agent_.ran) core::obj_set(out, "agent", section_json(suite.agent_));
    if (suite.clients_out_.type == Json::Obj) core::obj_set(out, "clients", suite.clients_out_);

    if (!o.compare.empty()) {
        try {
            const Json prev = core::json_parse(read_all(fs::path(o.compare) / "results.json"));
            md << "\n## Compared with\n\n| test | decode tok/s (this) | decode tok/s (" << fs::path(o.compare).filename().string() << ") |\n|---|---|---|\n";
            const std::pair<const char*, const char*> keys[] = {{"chat", "chat"}, {"4K prompt", "4K"}, {"16K prompt", "16K"}, {"32K prompt", "32K"}, {"agent", "agent"}};
            for (size_t i = 0; i < 5; ++i) {
                const Section* sec = rows[i].second;
                const Json* other = core::json_get(prev, keys[i].second);
                const Json* dec = other ? core::json_get(*other, "decode_tps") : nullptr;
                const auto med = dec && dec->type == Json::Obj ? num_of(*dec, "median") : std::nullopt;
                if (sec->ran && sec->ok() && med)
                    md << "| " << keys[i].first << " | " << fmt_stat(sec->med(&Digest::decode_tps), 1) << " | " << fmt_num(med, 1) << " |\n";
            }
        } catch (...) {
            s.out().warn("could not read " + (fs::path(o.compare) / "results.json").string());
        }
    }

    const fs::path root = c.str("BENCH_DIR", (s.paths().home / "bench" / "results").string());
    fs::path out_dir = o.out.empty() ? unique_dir(root, today() + "-community-" + (slug(gpu, 24).empty() ? "cpu" : slug(gpu, 24)) + "-" +
                                                            (slug(fs::path(label).stem().string(), 60).empty() ? "model" : slug(fs::path(label).stem().string(), 60)))
                                     : fs::path(o.out);
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    std::ofstream(out_dir / "README.md", std::ios::trunc) << red(md.str());
    std::ofstream(out_dir / "results.json", std::ios::trunc) << core::json_dump(out) << "\n";
    if (o.url.empty()) {
        std::ifstream lg(s.log_file("main"));
        std::vector<std::string> lines;
        std::string l;
        while (std::getline(lg, l)) { lines.push_back(l); if (lines.size() > 80) lines.erase(lines.begin()); }
        if (!lines.empty()) {
            std::string tail;
            for (const auto& x : lines) tail += x + "\n";
            std::ofstream(out_dir / "server.log", std::ios::trunc) << red(tail);
        }
    }
    s.out().ok("report", out_dir.string());
    for (const auto& [name, sec] : rows) {
        if (!sec->ran || !sec->ok() || std::string(name) == "32K prompt") continue;
        char b[200];
        std::snprintf(b, sizeof b, "    %-12s decode %6s tok/s   prompt %7s tok/s   ttft %ss", name, fmt_stat(sec->med(&Digest::decode_tps), 1).c_str(),
                      fmt_stat(sec->med(&Digest::prompt_tps), 1).c_str(), fmt_stat(sec->med(&Digest::ttft_s), 2).c_str());
        std::cout << b << "\n";
    }
    if (acc) std::printf("    draft accept %.1f%% (%.0f/%.0f)\n", 100.0 * acc->accepted / acc->offered, acc->accepted, acc->offered);
    if (sampler.vram() > 0) std::cout << "    VRAM peak " << sampler.vram() << " MiB" << (sampler.rss() > 0 ? ", server RSS " + std::to_string(sampler.rss()) + " MiB" : "") << "\n";

    if (started && !o.keep) {
        s.out().dim("  stopping the server this bench started (--keep leaves it up)");
        s.stop_one("main");
    } else if (started) {
        s.out().note("left up as asked: the model is loaded, so `ariadne limbo start` will refuse while it runs");
    }
    return 0;
}

}  // namespace bench
}  // namespace limbo
}  // namespace dd
