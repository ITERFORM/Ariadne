#include "dd/cron/executor.hpp"

#include "dd/core/json.hpp"
#include "dd/core/logbook.hpp"
#include "dd/tools/interrupt.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace dd {
namespace cron {

using namespace core;

static std::string sget(const Json& o, const std::string& k) {
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Str) return v->str;
    return "";
}

JobOutcome AgentJobExecutor::run(const Json& job, const std::string& prompt,
                                 const std::atomic<bool>& stop,
                                 std::atomic<long long>* activity_ms) {
    JobOutcome outcome;
    const auto t0 = std::chrono::steady_clock::now();
    auto touch = [activity_ms] {
        if (activity_ms)
            activity_ms->store(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::system_clock::now().time_since_epoch())
                                   .count(),
                               std::memory_order_relaxed);
    };

    // Fresh agent, fresh transcript: the delegate child's assembly.
    agent::Agent child;
    agent::HttpLLM llm;
    std::atomic<long> prompt_tokens{0}, completion_tokens{0};
    if (deps_.assigned) {
        child.llm = deps_.assigned;
    } else if (deps_.transport) {
        llm.base_url = deps_.transport->base_url;
        llm.model = deps_.transport->model;
        llm.api_key = deps_.transport->api_key;
        llm.extra = json_deep_copy(deps_.transport->extra);
        llm.effort = deps_.transport->effort;
        llm.shape = deps_.transport->shape;
        llm.attribution = deps_.transport->attribution;
        llm.stall_timeout_s = deps_.transport->stall_timeout_s;
        llm.retry_attempts = deps_.transport->retry_attempts;
        llm.retry_budget_ms = deps_.transport->retry_budget_ms;
        llm.retry_after_cap_ms = deps_.transport->retry_after_cap_ms;
        llm.retry_after_total_ms = deps_.transport->retry_after_total_ms;
        // NOT on_wait and no on_begin/on_content/on_reasoning: the parent's hooks write into
        // the interactive stream; a cron run is quiet and its activity feeds the watchdog.
        child.llm = &llm;
    } else {
        return {false, "", "cron executor: no transport configured"};
    }

    llm.cancel = tools::interrupt::current();
    llm.cancel2 = &stop;
    llm.on_stats = [&](const agent::CallStats& s) {
        touch();
        prompt_tokens += s.prompt_tokens;
        completion_tokens += s.completion_tokens;
    };
    llm.on_reasoning = [&touch](const std::string&) { touch(); };
    llm.on_content = [&touch](const std::string&) { touch(); };
    child.step_hook = [&touch](const core::Message&) { touch(); };

    const std::string job_model = sget(job, "model");
    const std::string job_base = sget(job, "base_url");
    if (!job_model.empty()) {
        child.model = job_model;
        llm.model = job_model;
    } else {
        child.model = deps_.fallback_model.empty() ? llm.model : deps_.fallback_model;
    }
    if (!job_base.empty() && !deps_.assigned) llm.base_url = job_base;

    child.max_iterations = deps_.max_iterations;
    child.quiet_mode = true;
    child.registry = deps_.registry;
    child.gate = deps_.gate;
    child.system_prompt = deps_.system_prompt;

    tools::interrupt::clear();   // a leftover Esc from a previous turn must not abort this run
    running_.store(&child, std::memory_order_release);
    try {
        outcome.final_response = child.run(prompt);
        outcome.success = true;
    } catch (const std::exception& e) {
        outcome.success = false;
        outcome.error = e.what();
    }
    running_.store(nullptr, std::memory_order_release);

    const double dur = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    const bool interrupted = child.interrupt_requested.load(std::memory_order_acquire);
    if (interrupted && outcome.success && stop.load(std::memory_order_acquire)) {
        // The watchdog or shutdown ended the run mid-flight: the runner turns this into a
        // timeout failure or drops the bookkeeping; either way the answer is not the answer.
        outcome.success = false;
        outcome.error = "interrupted";
        outcome.final_response.clear();
    }
    dd::core::logbook::emit(dd::core::logbook::Level::Info, "cron", "job.run",
                            {{"id", dd::core::logbook::S(sget(job, "id"))},
                             {"status", dd::core::logbook::S(outcome.success ? "completed" : (interrupted ? "interrupted" : "error"))},
                             {"ms", dd::core::logbook::N(dur * 1000.0)},
                             {"iterations", dd::core::logbook::N((double)child.api_call_count)},
                             {"in_tokens", dd::core::logbook::N((double)prompt_tokens.load())},
                             {"out_tokens", dd::core::logbook::N((double)completion_tokens.load())}});
    return outcome;
}

void AgentJobExecutor::interrupt() {
    if (agent::Agent* a = running_.load(std::memory_order_acquire))
        a->interrupt_requested.store(true, std::memory_order_release);
}

}  // namespace cron
}  // namespace dd
