#pragma once
// dd/limbo/bench.hpp — `ariadne limbo bench`: what this box actually serves with, measured
// over the real HTTP API, written as a report to keep or share.
//
//   chat        three short questions (~100 tokens out): decode tok/s, time to first token
//   4K / 16K    a prompt of about that many tokens, fresh every run so no cache helps
//   agent       a coding-agent session: system prompt + four tools, five turns growing the
//               conversation by ~1,200 words each: how much the cache reused, TTFT per turn
//   --clients N N requests at once: total tok/s, p50/p95 latency
//
// Speeds are the engine's own (`timings`); TTFT and latency are measured here, because that is
// what a caller feels. Thinking is off unless --think, so runs stay short and comparable.
// py: daedalus_cli/stack_bench.py

#include <optional>
#include <string>
#include <vector>

#include "dd/core/json.hpp"
#include "dd/limbo/stack.hpp"

namespace dd {
namespace limbo {
namespace bench {

struct Options {
    std::string profile, url, out, compare;
    int runs = 3;
    int clients = 0;
    bool long_ = false, think = false, quick = false, no_start = false, keep = false;
};

int cmd_bench(const Stack& s, const Options& o);

// ---- testable pieces -------------------------------------------------------------------
// About `words` words plus a short question; `salt` makes every run's prompt new.
std::string long_prompt(int words, unsigned salt);
// A deterministic pseudo-file: the agent session's growth per turn.
std::string file_chunk(int turn, int words);

struct ChatResult {
    double first = -1;            // seconds to the first produced piece; -1 = none
    double total = 0;
    std::string text, err;
    core::Json usage, timings;    // Null when the stream did not carry them
    int pieces = 0;
};
// Feed one SSE `data:` payload into the result. False on [DONE].
bool sse_event(const std::string& payload, double t_since_start, ChatResult& r);

struct Digest {
    bool ok = false;
    std::string err;
    std::optional<double> ttft_s, total_s, prompt_tokens, reused_tokens, read_tokens, generated_tokens,
        prompt_tps, decode_tps, prompt_ms, decode_ms, drafts_offered, drafts_accepted;
    std::string decode_source;    // engine | client
};
Digest digest(const ChatResult& r);

struct Stat { double median = 0, min = 0, max = 0; int n = 0; };
std::optional<Stat> stat(const std::vector<std::optional<double>>& values);
std::optional<double> pctl(std::vector<std::optional<double>> values, double p);
// Home paths, the user and the host name do not travel in a report.
std::string redact(const std::string& text, const std::string& home, const std::string& user, const std::string& host);
std::string slug(const std::string& text, size_t limit);

}  // namespace bench
}  // namespace limbo
}  // namespace dd
