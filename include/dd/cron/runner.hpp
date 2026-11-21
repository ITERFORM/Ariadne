#pragma once
// dd/cron/runner.hpp — cron job EXECUTION (port of cron/scheduler.py). jobs.hpp owns the
// schedule and the store; this is what fires the due jobs: the gateway's background ticker,
// the per-job pipeline (prompt -> run -> output doc -> delivery -> bookkeeping), the pre-run
// script with its path confinement, and the delivery-target resolution.
//
// The pipeline is faithful to scheduler.py::tick():
//   under the tick lock -> get_due_jobs() -> per job:
//     claim the turn gate (park while an interactive turn holds it, no schedule change)
//     advance_next_run(id) BEFORE the run (a crash mid-run leaves the schedule moved on)
//     build the prompt (cron hint + pre-run script + skills), run it on an executor
//     save the output doc; deliver (skipped on [SILENT]); mark_job_run(...)
//
// Execution is injected (JobExecutor), so this unit carries no agent or wire and tests run
// the whole pipeline against a fake. The gateway supplies the real one (cron/executor.hpp:
// a fresh ephemeral agent per job, the delegate child's assembly). Delivery is injected the
// same way (DeliverySink): in-tree there are no platform adapters yet, so a resolved
// platform target records "not configured/enabled" as last_delivery_error exactly where the
// Python fell through to its standalone send.
//
// Concurrency: no mutex and no condition_variable (the house rule). The tick serializes
// across processes with flock (core/fslock.hpp, non-blocking: a tick that finds the lock
// busy skips). One job runs on a worker thread while this thread is the watchdog — the
// hand-off of the outcome is one release/acquire flag, and the watchdog's stop reaches the
// run through an atomic plus JobExecutor::interrupt(). Interactive turns and cron turns
// exclude each other through one shared atomic claim (the serve layer's, see chat_api.hpp).
//
// Datetime semantics come from cron/schedule.hpp: minutes-since-epoch UTC.

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>

#include "dd/core/config.hpp"
#include "dd/core/json.hpp"

namespace dd {
namespace cron {

// ---- what a job's run produced ----
// The executor answers the prompt; the runner owns the output doc, delivery and bookkeeping.
struct JobOutcome {
    bool success = false;
    std::string final_response;   // the model's answer; on failure empty and error says why
    std::string error;
};

// Runs one job's prompt to an answer. `stop` is set by the watchdog (inactivity limit or
// shutdown): the executor must end the run when it sees it (wire cancel, loop poll, shell
// cancel). `activity_ms` is a clock the executor touches whenever the model or a tool moves
// (millis since epoch); the watchdog fires when it goes quiet. `interrupt()` is the same
// stop from the outside, for what `stop` cannot reach (the loop's between-step poll).
struct JobExecutor {
    virtual ~JobExecutor() = default;
    virtual JobOutcome run(const core::Json& job, const std::string& prompt,
                           const std::atomic<bool>& stop,
                           std::atomic<long long>* activity_ms) = 0;
    virtual void interrupt() {}
};

// Where a non-local delivery goes. nullopt: delivered; else the error recorded as
// last_delivery_error (a delivery failure never fails the run, as in the Python).
struct DeliverySink {
    virtual ~DeliverySink() = default;
    virtual std::optional<std::string> deliver(const core::Json& target,
                                               const std::string& content) = 0;
};

// The shipped sink: nowhere to send. A resolved platform target reports the error the
// Python reported for an unconfigured platform; the owner swaps in a real sender when the
// platform adapters land.
struct NullDelivery : DeliverySink {
    std::optional<std::string> deliver(const core::Json& target, const std::string&) override;
};

// A sink that forwards to a callback (the gateway's seam for live adapters later).
struct FnDelivery : DeliverySink {
    std::function<std::optional<std::string>(const core::Json&, const std::string&)> fn;
    std::optional<std::string> deliver(const core::Json& target, const std::string& content) override {
        return fn ? fn(target, content) : NullDelivery().deliver(target, content);
    }
};

// ---- delivery resolution (port of scheduler.py::_resolve_delivery_target) ----
// deliver=:
//   "local"                  -> nullopt (the output files ARE the delivery)
//   "origin"                 -> the job's origin {platform, chat_id, thread_id}, else the
//                               first of matrix/telegram/discord/slack/bluebubbles with a
//                               {PLATFORM}_HOME_CHANNEL in the environment, else nullopt
//   "platform:rest"          -> explicit target (rest is the chat id, optionally with
//                               thread); the channel directory is a caller concern
//   "platform" (bare)        -> the origin when it matches, else {PLATFORM}_HOME_CHANNEL,
//                               else nullopt for a name outside the known platform set
// `env(name)` is injectable for tests; the default reads the process environment.
std::optional<core::Json> resolve_delivery_target(
    const core::Json& job,
    const std::function<std::string(const std::string&)>& env =
        [](const std::string& k) -> std::string {
            const char* v = ::getenv(k.c_str());
            return v ? std::string(v) : std::string();
        });

// The delivery envelope (cron.wrap_response, default on).
std::string wrap_delivery(const std::string& content, const std::string& task_name);
// "[SILENT]" anywhere in the stripped, uppercased answer suppresses delivery (the Python's
// SILENT_MARKER rule).
bool is_silent(const std::string& final_response);

// ---- pre-run script (port of scheduler.py::_run_job_script) ----
// A job's `script` is a data-collection step that runs before the model sees the prompt.
// Path traversal is refused: relative names resolve against `scripts_dir` and absolute or
// ~-prefixed names must still land inside it. Runs argv-exact (no shell), stdout and stderr
// captured apart, wall-clock bounded, the whole process group killed at the deadline.
// Returns (success, output); on failure output carries the error text for the model.
std::pair<bool, std::string> run_job_script(const std::string& script_path,
                                            const std::filesystem::path& scripts_dir,
                                            const std::string& interpreter,
                                            long long timeout_s);

// How a skill's body is loaded for the prompt ("name" -> SKILL.md body). Empty loader:
// every listed skill is "not found" and the prompt says so.
using SkillLoader = std::function<std::optional<std::string>(const std::string& name)>;

struct PromptBuildOptions {
    std::string interpreter = "python3";   // the scripts' interpreter
    long long script_timeout_s = 120;
};

// The prompt the model sees (port of _build_job_prompt): the cron hint first ([SYSTEM: ...
// DELIVERY ... SILENT ...]), then the pre-run script's output (or its error), then each
// skill's body, then the operator's instruction. The hint is what keeps a job from trying
// to deliver its own answer and what teaches it the [SILENT] convention.
std::string build_job_prompt(const core::Json& job, const SkillLoader& skills,
                             const PromptBuildOptions& opt);

// The run's output document (port of run_job's template): the job, the prompt, the
// response — or the error under (FAILED).
std::string output_doc(const core::Json& job, const std::string& prompt,
                       const JobOutcome& outcome);

// ---- options (port of the Python config surface: cron.* + DD_CRON_*) ----
struct SchedulerOptions {
    long long tick_seconds = 60;        // cron.tick_seconds
    long long timeout_s = 600;          // cron.timeout_seconds / DD_CRON_TIMEOUT, 0 = no limit
    long long script_timeout_s = 120;   // cron.script_timeout_seconds / DD_CRON_SCRIPT_TIMEOUT
    bool wrap_response = true;          // cron.wrap_response
    std::string interpreter = "python3";  // cron.script_interpreter
};
SchedulerOptions options_from(const core::Config& cfg);

// ---- the gateway's ticker ----
class Scheduler {
public:
    Scheduler(SchedulerOptions opt, JobExecutor& exec, DeliverySink& sink);

    // Hooks the owner may set before run(): where delivery targets resolve their environment,
    // how a skill body loads, what a tick prints (the CLI ticks verbosely, the gateway does
    // not), and the one-turn-at-a-time claim shared with the serve layer. A due job parks
    // (stays due, nothing runs) while the claim is held, instead of interleaving on a
    // single-sequence endpoint.
    std::function<std::string(const std::string&)> env;
    SkillLoader skill_loader;
    std::function<void(const std::string&)> note;
    std::atomic<bool>* claim = nullptr;

    // One pass under the tick lock: the number of jobs run (0 when another tick holds the
    // lock, none are due, or `stop` fires mid-pass). `stop` is the process signal: it
    // interrupts a running job, which is then left unmarked (its schedule was already
    // advanced — the same bookkeeping a crash leaves behind).
    int tick(const std::atomic<int>* stop = nullptr);
    // The gateway's background loop: tick every tick_seconds until `stop`.
    void run(const std::atomic<int>& stop);

private:
    std::optional<std::string> deliver(const core::Json& job, const std::string& content);

    SchedulerOptions opt_;
    JobExecutor& exec_;
    DeliverySink& sink_;
};

}  // namespace cron
}  // namespace dd
