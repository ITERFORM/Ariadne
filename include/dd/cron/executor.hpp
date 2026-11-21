#pragma once
// dd/cron/executor.hpp — what actually runs one cron job's prompt: a fresh ephemeral
// dd::agent::Agent per run (own transcript, no ContextEngine — a job is one question, one
// answer) on its own transport copy. The assembly is the delegate child's (delegate_tool.cpp),
// which is the tree's existing idiom for "a second agent inside one process, quiet":
//   - the wire is copied field by field from the gateway's transport (never by value: the
//     copy must not carry the parent's hooks, which write into the interactive stream),
//   - NOT on_wait (the same reason a delegate child has none),
//   - llm.cancel is the process interrupt (a signal ends a cron call), llm.cancel2 is the
//     runner's per-job stop (the inactivity watchdog and shutdown),
//   - quiet_mode, no tape, no steering.
//
// A job may name its own model/base_url (the Python resolved a whole runtime provider per
// job; here the same endpoint answers under another model name — provider routing is not
// modeled on this wire and is recorded as a divergence in docs/PARITY.md).

#include <atomic>
#include <string>

#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/cron/runner.hpp"
#include "dd/tools/registry.hpp"

namespace dd {
namespace cron {

struct AgentJobExecutorDeps {
    const agent::HttpLLM* transport = nullptr;   // the gateway's wire; copied field by field
    agent::LLMClient* assigned = nullptr;        // a pre-built client (a fake in tests) wins
    tools::ToolRegistry* registry = nullptr;
    agent::ToolGate* gate = nullptr;             // the operator's gate (a cron run is not a device turn)
    std::string system_prompt;                   // the assembled harness rules (agent.system_prompt)
    std::string fallback_model;                  // the gateway's model when the job names none
    int max_iterations = 90;
};

class AgentJobExecutor : public JobExecutor {
public:
    explicit AgentJobExecutor(AgentJobExecutorDeps deps) : deps_(std::move(deps)) {}

    JobOutcome run(const core::Json& job, const std::string& prompt,
                   const std::atomic<bool>& stop, std::atomic<long long>* activity_ms) override;
    // The watchdog/shutdown stop, for what `stop` cannot reach: the loop's between-step poll.
    void interrupt() override;

private:
    AgentJobExecutorDeps deps_;
    std::atomic<agent::Agent*> running_{nullptr};
};

}  // namespace cron
}  // namespace dd
