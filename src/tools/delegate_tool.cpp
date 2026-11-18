// Port of tools/delegate_tool.py — the parent side of sub-agents.
// The child is a fresh dd::agent::Agent (the loop is already ported there);
// this file is the spawn, the focused prompt, the depth/width limits, the
// concurrent batch and the structured report the parent's model reads.
#include "dd/tools/delegate_tool.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dd/agent/agent.hpp"
#include "dd/agent/http_llm.hpp"
#include "dd/core/logbook.hpp"
#include "dd/tools/interrupt.hpp"
#include "dd/tools/fs_tools.hpp"
#include "dd/tools/patch_apply.hpp"
#include "dd/tools/registry.hpp"
#include "dd/tools/skill_provenance.hpp"
#include "dd/tools/todo_tool.hpp"

using namespace dd::core;

namespace dd {
namespace tools {

namespace {

JsonSP jsp(Json v) { return std::make_shared<Json>(std::move(v)); }

// ---- the child's tools: the file and terminal set, every handler thin over the
// shared primitives (fs_read/fs_write/fs_list/fs_edit, run_shell/start_job,
// job_call) exactly as the thin handlers in the two mains are. Children do not
// share the parent's TodoStore (a sub-agent's plan ledger is its own) and do
// not write the parent's ephemeral prompt. ----

struct ChildToolDeps {
    std::string skills_root;
    ShellPolicy shell_policy;
    int child_depth = 0;                     // delegate_task registers only under the ceiling
    const DelegateToolDeps* delegate = nullptr;
    const std::set<std::string>* filter = nullptr;   // requested toolsets; empty = all
    const std::atomic<bool>* job_stop = nullptr;     // this batch's kill, beside the turn's Esc
    bool wanted(const char* toolset) const {
        return !filter || filter->empty() || filter->count(toolset);
    }
};

void register_child_tools(ToolRegistry& reg, const ChildToolDeps& d) {
    ToolEntry e;
    if (d.wanted("file")) {
        e.name = "read_file";
        e.toolset = "file";
        e.schema = read_file_schema();
        e.handler = [](const Json& a) { return fs_read(a); };
        reg.register_tool(e);

        e.name = "write_file";
        e.schema = write_file_schema();
        e.handler = [](const Json& a) { return fs_write(a); };
        reg.register_tool(e);

        e.name = "list_dir";
        e.schema = list_dir_schema();
        e.handler = [](const Json& a) { return fs_list(a); };
        reg.register_tool(e);

        e.name = "edit_file";
        e.schema = edit_tool_schema();
        e.handler = [skills_root = d.skills_root](const Json& a) {
            ApplyOptions opt;
            opt.skills_root = skills_root;
            const WriteOriginScope scope("model_edit");
            return fs_edit(a, opt);
        };
        reg.register_tool(e);
    }

    if (d.wanted("terminal")) {
        e.name = "run_terminal";
        e.toolset = "terminal";
        Json rt = json_arr();
        arr_push(rt, json_str("cmd"));
        Json props = json_obj({{"cmd", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                     {"description", jsp(json_str("command to run"))}}))},
                               {"background", jsp(json_obj({{"type", jsp(json_str("boolean"))},
                                                            {"description", jsp(json_str("true: start it and return at once with a job id"))}}))},
                               {"intent", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                        {"description", jsp(json_str("what you are doing, in a few words (optional)"))}}))}});
        e.schema = json_obj({{"description", jsp(json_str(
            "Run a shell command. A guard refuses obviously destructive commands; it is not a sandbox. "
            "Returns exit code + stdout. A foreground command is killed after "
            + std::to_string(kForegroundTimeoutMs / 1000) + " s; long work runs with background:true and is "
            "looked at with the job tool."))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(props))},
                                                          {"required", jsp(std::move(rt))}}))}});
        e.handler = [policy = d.shell_policy, stop = d.job_stop](const Json& a) {
        const Json* c = json_get(a, "cmd");
        if (!c || c->type != Json::Str || c->str.empty()) return tool_error("cmd required");
        const Json* bg = json_get(a, "background");
        if (bg && bg->type == Json::Bool && bg->b) return with_job_notices(start_job(c->str, policy));
        ShellOptions opt;
        opt.cancel = interrupt::current();
        opt.cancel2 = stop;   // kill this job, not the turn
        return with_job_notices(run_shell(c->str, opt, policy));
    };
        reg.register_tool(e);

        e.name = "job";
        e.toolset = "terminal";
        Json actions = json_arr();
        for (const char* a : {"status", "wait", "kill", "list"}) arr_push(actions, json_str(a));
        Json jprops = json_obj({{"action", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                        {"enum", jsp(std::move(actions))}}))},
                               {"job", jsp(json_obj({{"type", jsp(json_str("integer"))},
                                                     {"description", jsp(json_str("the id run_terminal returned"))}}))},
                               {"wait_s", jsp(json_obj({{"type", jsp(json_str("integer"))}}))}});
        Json rq = json_arr();
        arr_push(rq, json_str("action"));
        e.schema = json_obj({{"description", jsp(json_str("Look at a background command started with run_terminal background:true."))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(jprops))},
                                                          {"required", jsp(std::move(rq))}}))}});
        e.handler = [stop = d.job_stop](const Json& a) {
        auto str = [&](const char* k) {
            const Json* v = json_get(a, k);
            return v && v->type == Json::Str ? v->str : std::string();
        };
        auto num = [&](const char* k) {
            const Json* v = json_get(a, k);
            return v && v->type == Json::Num ? static_cast<int>(v->num) : 0;
        };
        JobOptions opt;
        opt.cancel = interrupt::current();
        opt.cancel2 = stop;   // a killed job stops waiting too
        return with_job_notices(job_call(str("action"), num("job"), num("wait_s"), opt));
    };
        reg.register_tool(e);
    }

    e.name = "todo";
    e.toolset = "";   // an agent-loop tool, not a toolset the caller filters
    {
        Json item = json_obj({{"id", jsp(json_obj({{"type", jsp(json_str("string"))}}))},
                              {"content", jsp(json_obj({{"type", jsp(json_str("string"))}}))},
                              {"status", jsp(json_obj({{"type", jsp(json_str("string"))},
                                                       {"enum", jsp(json_arr({jsp(json_str("pending")), jsp(json_str("in_progress")),
                                                                              jsp(json_str("completed")), jsp(json_str("cancelled"))}))}}))}});
        Json props = json_obj({{"todos", jsp(json_obj({{"type", jsp(json_str("array"))},
                                                       {"items", jsp(std::move(item))}}))},
                               {"merge", jsp(json_obj({{"type", jsp(json_str("boolean"))}}))}});
        e.schema = json_obj({{"description", jsp(json_str("Work ledger for a multi-step task. The sub-agent's own ledger: "
                                                          "provide `todos` to write the whole list, omit to read it back; "
                                                          "merge=true updates only the items named."))},
                             {"parameters", jsp(json_obj({{"type", jsp(json_str("object"))},
                                                          {"properties", jsp(std::move(props))}}))}});
    }
    e.handler = [store = std::make_shared<TodoStore>()](const Json& a) {
        const Json* t = json_get(a, "todos");
        const Json* m = json_get(a, "merge");
        return todo_tool(t ? *t : Json{}, todo_merge_flag(m), *store);
    };
    reg.register_tool(e);

    // Delegation, one hop under the ceiling: a child at depth kDelegateMaxDepth
    // gets no delegate_task, so the tree ends there (delegate_tool.py checks
    // parent depth at call time; here the tool simply is not there to call).
    if (d.child_depth < kDelegateMaxDepth && d.delegate) {
        e.name = "delegate_task";
        e.toolset = "";   // agent-loop tool: never filtered away (delegate_tool.py _AGENT_LOOP_TOOLS)
        e.schema = delegate_tool_schema();
        e.handler = make_delegate_tool(*d.delegate);
        reg.register_tool(e);
    }
}

// ---- the parent's facts, copied ----
// A background job outlives the turn that started it, and in a nested tree it can outlive the child
// Agent that started it too. So the job never dereferences `DelegateToolDeps::parent`: the three
// things a child takes from it are copied here, at start, while the parent is certainly alive.
struct ParentFacts {
    std::string model;
    agent::ToolGate* gate = nullptr;
    int depth = 0;
};

ParentFacts facts_of(const DelegateToolDeps& deps) {
    ParentFacts f;
    if (deps.parent) {
        f.model = deps.parent->model;
        f.gate = deps.parent->gate;
        f.depth = deps.parent->delegate_depth;
    }
    return f;
}

// ---- the job depot: background delegations, alive across turns ----
// A fixed pool, never a container that grows: eight jobs, one per slot, each with three result cells
// and its own stop flag. The state machine is the whole synchronisation:
//
//   Free --(start, CAS)--> Starting --(fields written, workers up)--> Running
//   Running --(last child finishes)--> Finished --(collect/kill joins)--> Free
//
// Only the starter touches `workers`, `spec` and `goals` (it owns the slot from Free to Running);
// only the collector touches `workers` again (it owns it from Finished to Free); a child writes its
// own result cell and bumps `done` with release, and a reader trusts a cell only after seeing
// `state == Finished` (acquire). No mutex, no shared_ptr in the hot path, nothing destroyed while a
// worker might still touch it. Max width is kDelegateMaxChildren, so the cells are an array, not a
// vector.
constexpr int kJobFree = 0, kJobStarting = 1, kJobRunning = 2, kJobFinished = 3;   // kDelegateMaxJobs lives in the header

struct Job {
    std::atomic<int> state{kJobFree};
    std::atomic<int> id{0};
    std::atomic<int> n{0};
    std::atomic<int> done{0};
    std::atomic<bool> stop{false};
    std::atomic<bool> killed{false};
    std::atomic<bool> joined{true};
    std::atomic<long long> started_ms{0};
    std::atomic<long long> ended_ms{0};
    std::array<Json, kDelegateMaxChildren> results;
    std::array<std::atomic<int>, kDelegateMaxChildren> child_state;   // 0 pending, 1 running, 2 done
    std::array<std::string, kDelegateMaxChildren> goals;
    std::vector<std::thread> workers;
    std::optional<agent::HttpLLM> wire;   // the wire by value: no pointer into a caller's stack
    DelegateToolDeps spec;                // everything a child needs, `parent` deliberately null
    ParentFacts facts;
    std::set<std::string> toolsets;
};

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

void join_job(Job& j) {
    bool expected = false;
    if (!j.joined.compare_exchange_strong(expected, true)) return;   // one joiner, ever
    for (auto& t : j.workers)
        if (t.joinable()) t.join();
    j.workers.clear();
}

struct Pool {
    std::array<Job, kDelegateMaxJobs> jobs;
    std::atomic<int> next_id{1};
    // At exit the children are told to stop and joined here. Joining is what keeps the process from
    // dying with a joinable std::thread (std::terminate) and from leaving a worker reading a
    // half-destroyed transport. A child that ignores the flag is bounded by its tool's own timeout.
    ~Pool() {
        bool any = false;
        for (auto& j : jobs) {
            const int st = j.state.load(std::memory_order_acquire);
            if (st == kJobStarting || st == kJobRunning || st == kJobFinished) {
                j.stop.store(true, std::memory_order_release);
                any = true;
            }
        }
        if (any) interrupt::signal();   // the shell tool watches this one
        for (auto& j : jobs) join_job(j);
    }
};

Pool& pool() {
    static Pool p;   // constructed on the first delegation, destroyed at exit
    return p;
}

// A slot: Free outright, or a Finished job whose workers have been joined (its result was already
// collected, or the operator never asked). Running jobs are never stolen.
Job* take_slot() {
    Pool& p = pool();
    for (auto& j : p.jobs) {
        int expected = kJobFree;
        if (j.state.compare_exchange_strong(expected, kJobStarting)) return &j;
    }
    for (auto& j : p.jobs) {
        if (j.state.load(std::memory_order_acquire) != kJobFinished) continue;
        join_job(j);   // finished: returns at once
        int expected = kJobFinished;
        if (j.state.compare_exchange_strong(expected, kJobStarting)) {
            j.joined.store(true, std::memory_order_release);
            return &j;
        }
    }
    return nullptr;
}

Job* find_job(int id) {
    Pool& p = pool();
    for (auto& j : p.jobs) {
        const int st = j.state.load(std::memory_order_acquire);
        if (st != kJobFree && st != kJobStarting && j.id.load(std::memory_order_relaxed) == id) return &j;
    }
    return nullptr;
}

const char* state_word(const Job& j) {
    if (j.killed.load(std::memory_order_acquire)) return "killed";
    switch (j.state.load(std::memory_order_acquire)) {
        case kJobStarting: return "starting";
        case kJobRunning: return "running";
        case kJobFinished: return "done";
        default: return "free";
    }
}

Json job_view(const Job& j) {
    Json o = json_obj();
    obj_set(o, "job_id", json_num(static_cast<double>(j.id.load(std::memory_order_relaxed))));
    obj_set(o, "state", json_str(state_word(j)));
    obj_set(o, "total", json_num(static_cast<double>(j.n.load(std::memory_order_relaxed))));
    obj_set(o, "done", json_num(static_cast<double>(j.done.load(std::memory_order_acquire))));
    const long long s = j.started_ms.load(std::memory_order_relaxed);
    const long long e = j.ended_ms.load(std::memory_order_acquire);
    const long long now = now_ms();
    obj_set(o, "elapsed_seconds", json_num(static_cast<double>((e ? e : now) - s) / 1000.0));
    Json ch = json_arr();
    const int n = j.n.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        Json c = json_obj();
        obj_set(c, "task_index", json_num(i));
        const int cs = j.child_state[static_cast<size_t>(i)].load(std::memory_order_acquire);
        obj_set(c, "state", json_str(cs == 2 ? "done" : cs == 1 ? "running" : "pending"));
        if (i < static_cast<int>(j.goals.size()) && !j.goals[static_cast<size_t>(i)].empty())
            obj_set(c, "goal", json_str(logbook::head(j.goals[static_cast<size_t>(i)], 120)));
        arr_push(ch, c);
    }
    obj_set(o, "children", ch);
    return o;
}

// ---- one child, start to report ----

Json child_entry(int task_index, const std::string& status, const std::string& summary,
                 int api_calls, double duration_s, const std::string& model,
                 const std::string& exit_reason, long prompt_tokens, long completion_tokens,
                 const std::string& error) {
    Json t = json_obj({{"input", jsp(json_num(static_cast<double>(prompt_tokens)))},
                       {"output", jsp(json_num(static_cast<double>(completion_tokens)))}});
    Json e = json_obj({{"task_index", jsp(json_num(task_index))},
                       {"status", jsp(json_str(status))},
                       {"summary", summary.empty() ? jsp(Json{}) : jsp(json_str(summary))},
                       {"api_calls", jsp(json_num(api_calls))},
                       {"duration_seconds", jsp(json_num(std::round(duration_s * 100.0) / 100.0))},
                       {"model", model.empty() ? jsp(Json{}) : jsp(json_str(model))},
                       {"exit_reason", jsp(json_str(exit_reason))},
                       {"tokens", jsp(std::move(t))}});
    if (!error.empty()) obj_set(e, "error", json_str(error));
    return e;
}

Json run_one_child(int task_index, const std::string& goal, const std::string& context,
                   const ParentFacts& facts, int max_iterations, const DelegateToolDeps& deps,
                   const std::set<std::string>& toolsets,
                   std::shared_ptr<agent::LLMClient> assigned,
                   std::atomic<bool>* job_stop) {
    using clock = std::chrono::steady_clock;
    const clock::time_point t0 = clock::now();
    const int child_depth = facts.depth + 1;

    // One line per child, on the thread that runs it: which task, how deep, how wide it may go and
    // what it was told to do. The batch line below pairs with it, so a reading can tell a child that
    // ran slowly from a batch that never started it.
    {
        using namespace dd::core;
        logbook::emit(logbook::Level::Info, "delegate", "child.start",
                      {{"task", logbook::N(static_cast<double>(task_index))},
                       {"depth", logbook::N(static_cast<double>(child_depth))},
                       {"max_iterations", logbook::N(static_cast<double>(max_iterations))},
                       {"goal", logbook::S(logbook::head(goal, 200))}});
    }

    if (interrupt::current() && interrupt::is_interrupted())
        return child_entry(task_index, "interrupted", "", 0, 0.0, "", "interrupted", 0, 0, "");
    if (job_stop && job_stop->load(std::memory_order_acquire))
        return child_entry(task_index, "killed", "", 0, 0.0, "", "killed", 0, 0, "");

    // Fresh child: own transcript (no ContextEngine), own transport and registry.
    agent::Agent child;
    agent::HttpLLM llm;
    std::atomic<long> prompt_tokens{0}, completion_tokens{0};
    if (assigned) {
        child.llm = assigned.get();   // owned by the handler; outlives the child's run
    } else if (deps.transport) {
        llm.base_url = deps.transport->base_url;
        llm.model = deps.transport->model;
        llm.api_key = deps.transport->api_key;
        llm.extra = json_deep_copy(deps.transport->extra);
        llm.effort = deps.transport->effort;
        llm.shape = deps.transport->shape;
        llm.attribution = deps.transport->attribution;
        llm.stall_timeout_s = deps.transport->stall_timeout_s;
        llm.retry_attempts = deps.transport->retry_attempts;
        llm.retry_budget_ms = deps.transport->retry_budget_ms;
        llm.retry_after_cap_ms = deps.transport->retry_after_cap_ms;
        llm.retry_after_total_ms = deps.transport->retry_after_total_ms;
        // NOT on_wait: the parent's closure captures the TUI's scene by reference,
        // and a child runs on a worker thread — three children waiting on their wire
        // (a 429, a stall, a backoff) would mutate the parent's scene concurrently
        // and shred the heap; the next std::thread spawn then dies reading freed
        // state (the parallel-batch segv of 2026-11-06). A child is quiet_mode:
        // its waits are internal, they paint nothing.
    }
    // Two authorities end this child's wire call: the turn's Esc (the parent's bound flag) and the
    // job's own kill. The second is what makes `kill` on one batch possible without ending the turn
    // that started it — and without touching the other batches.
    llm.cancel = interrupt::current();
    llm.cancel2 = job_stop;
    llm.on_stats = [&prompt_tokens, &completion_tokens](const agent::CallStats& s) {
        prompt_tokens += s.prompt_tokens;
        completion_tokens += s.completion_tokens;
    };

    ToolRegistry reg;
    DelegateToolDeps child_deps = deps;                 // the grandchild's wire is the same
    child_deps.parent = &child;
    ChildToolDeps ct{deps.skills_root, deps.shell_policy, child_depth, &child_deps, &toolsets, job_stop};
    register_child_tools(reg, ct);

    child.model = assigned ? facts.model : llm.model;
    child.max_iterations = max_iterations;
    child.quiet_mode = true;
    child.registry = &reg;
    if (!assigned) child.llm = &llm;
    child.gate = facts.gate;   // the parent's scope holds for children
    child.delegate_depth = child_depth;
    child.ephemeral_system_prompt = delegate_child_prompt(goal, context, deps.workspace_hint);

    try {
        const std::string summary = child.run(goal);
        const double dur = std::chrono::duration<double>(clock::now() - t0).count();
        const bool interrupted = interrupt::current() && interrupt::is_interrupted();
        const bool killed = !interrupted && job_stop && job_stop->load(std::memory_order_acquire);
        {
            using namespace dd::core;
            logbook::emit(logbook::Level::Info, "delegate", "child.end",
                          {{"task", logbook::N(static_cast<double>(task_index))},
                           {"depth", logbook::N(static_cast<double>(child_depth))},
                           {"status", logbook::S(interrupted ? "interrupted" : killed ? "killed" : (summary.empty() ? "failed" : "completed"))},
                           {"ms", logbook::N(dur * 1000.0)},
                           {"iterations", logbook::N(static_cast<double>(child.api_call_count))},
                           {"out_bytes", logbook::N(static_cast<double>(summary.size()))},
                           {"in_tokens", logbook::N(static_cast<double>(prompt_tokens.load()))},
                           {"out_tokens", logbook::N(static_cast<double>(completion_tokens.load()))}});
        }
        return child_entry(task_index,
                           interrupted ? "interrupted" : killed ? "killed" : (summary.empty() ? "failed" : "completed"),
                           summary, child.api_call_count, dur, child.model,
                           interrupted ? "interrupted" : killed ? "killed" : "completed",
                           prompt_tokens.load(), completion_tokens.load(),
                           summary.empty() && !interrupted ? "Subagent did not produce a response." : "");
    } catch (const std::exception& ex) {
        const double dur = std::chrono::duration<double>(clock::now() - t0).count();
        const bool interrupted = interrupt::current() && interrupt::is_interrupted();
        dd::core::logbook::emit(dd::core::logbook::Level::Error, "delegate", "child.end",
                                {{"task", dd::core::logbook::N(static_cast<double>(task_index))},
                                 {"depth", dd::core::logbook::N(static_cast<double>(child_depth))},
                                 {"status", dd::core::logbook::S(interrupted ? "interrupted" : "error")},
                                 {"ms", dd::core::logbook::N(dur * 1000.0)},
                                 {"iterations", dd::core::logbook::N(static_cast<double>(child.api_call_count))},
                                 {"why", dd::core::logbook::S(dd::core::logbook::head(ex.what(), 240))}});
        return child_entry(task_index, interrupted ? "interrupted" : "error", "", child.api_call_count,
                           dur, child.model, interrupted ? "interrupted" : "error",
                           prompt_tokens.load(), completion_tokens.load(),
                           interrupted ? "interrupted by the operator" : ex.what());
    }
}

std::string str_arg(const Json& o, const char* key) {
    const Json* v = json_get(o, key);
    return v && v->type == Json::Str ? v->str : std::string();
}

}  // namespace

std::string delegate_child_prompt(const std::string& goal, const std::string& context,
                                  const std::string& workspace_path) {
    std::string out =
        "You are a focused subagent working on a specific delegated task.\n"
        "\n"
        "YOUR TASK:\n" + goal;
    if (!context.empty()) out += "\n\nCONTEXT:\n" + context;
    if (!workspace_path.empty())
        out += "\n\nWORKSPACE PATH:\n" + workspace_path + "\n"
               "Use this exact path for local repository/workdir operations unless the task explicitly says otherwise.";
    out +=
        "\n\nComplete this task using the tools available to you. "
        "When finished, provide a clear, concise summary of:\n"
        "- What you did\n"
        "- What you found or accomplished\n"
        "- Any files you created or modified\n"
        "- Any issues encountered\n"
        "\n"
        "Important workspace rule: Never assume a repository lives at /workspace/... or any other "
        "container-style path unless the task/context explicitly gives that path. If no exact local "
        "path is provided, discover it first before issuing git/workdir-specific commands.\n"
        "\n"
        "Be thorough but concise -- your response is returned to the parent agent as a summary.";
    return out;
}

Json delegate_tool_schema() {
    auto str_prop = [](const std::string& desc) {
        return json_obj({{"type", jsp(json_str("string"))}, {"description", jsp(json_str(desc))}});
    };
    Json toolset_names = json_arr();
    arr_push(toolset_names, json_str("file"));
    arr_push(toolset_names, json_str("terminal"));

    Json goal = str_prop("the task for one sub-agent");
    Json context = str_prop("background the sub-agent needs (optional)");
    Json toolsets = json_obj();
    obj_set(toolsets, "type", json_str("array"));
    {
        Json items = json_obj();
        obj_set(items, "type", json_str("string"));
        obj_set(items, "enum", toolset_names);
        obj_set(toolsets, "items", items);
    }
    obj_set(toolsets, "description", json_str("which toolsets the sub-agent gets; omit for all"));

    Json task_props = json_obj();
    obj_set(task_props, "goal", goal);
    obj_set(task_props, "context", context);
    obj_set(task_props, "toolsets", toolsets);
    Json task = json_obj();
    obj_set(task, "type", json_str("object"));
    obj_set(task, "properties", task_props);
    obj_set(task, "required", json_arr({jsp(json_str("goal"))}));

    Json props = json_obj();
    obj_set(props, "goal", str_prop("single-task mode: the task for one sub-agent"));
    obj_set(props, "context", str_prop("single-task mode: background the sub-agent needs"));
    {
        Json t = json_obj();
        obj_set(t, "type", json_str("array"));
        obj_set(t, "items", json_obj({{"type", jsp(json_str("string"))}}));
        obj_set(t, "description", json_str("which toolsets the sub-agent(s) get; omit for all"));
        obj_set(props, "toolsets", t);
    }
    {
        Json tasks = json_obj();
        obj_set(tasks, "type", json_str("array"));
        obj_set(tasks, "items", task);
        obj_set(tasks, "description",
                json_str("batch mode: up to 3 tasks, run concurrently; results come back in task order"));
        obj_set(props, "tasks", tasks);
    }
    obj_set(props, "max_iterations", json_obj({{"type", jsp(json_str("integer"))},
                                               {"description", jsp(json_str("LLM calls a sub-agent may make (default 50)"))}}));

    {
        Json actions = json_arr();
        for (const char* a : {"run", "start", "status", "wait", "collect", "kill", "list"}) arr_push(actions, json_str(a));
        obj_set(props, "action", json_obj({{"type", jsp(json_str("string"))}, {"enum", jsp(std::move(actions))},
                                           {"description", jsp(json_str(
                                               "run (default): block until the children finish and return their results. "
                                               "start: return at once with a job_id while they run in the background. "
                                               "status / wait / collect / kill take job_id; list names the live jobs."))}}));
        obj_set(props, "job_id", json_obj({{"type", jsp(json_str("integer"))},
                                           {"description", jsp(json_str("the job a status/wait/collect/kill names"))}}));
        obj_set(props, "timeout_ms", json_obj({{"type", jsp(json_str("integer"))},
                                               {"description", jsp(json_str("wait: how long to block, default 60000, ceiling 600000"))}}));
        obj_set(props, "background", json_obj({{"type", jsp(json_str("boolean"))},
                                               {"description", jsp(json_str("true: same as action:\"start\""))}}));
    }


    Json params = json_obj();
    obj_set(params, "type", json_str("object"));
    obj_set(params, "properties", props);

    Json schema = json_obj();
    obj_set(schema, "description", json_str(
        "Spawn one or more focused sub-agents. One task: goal (+context). Several: tasks=[{goal,context},...] "
        "up to 3, run concurrently. Each sub-agent is a fresh agent loop with its own transcript and its own "
        "todo ledger, holding the file and terminal tools; it cannot see this conversation and reports back a "
        "summary only. Delegation ends " + std::to_string(kDelegateMaxDepth) + " levels deep. "
        "By default this call BLOCKS until every child is done; with action:\"start\" (or background:true) it "
        "returns at once with a job_id and the children keep running while you do other work — then "
        "action:\"status\" polls, action:\"wait\" blocks, action:\"collect\" takes the results, action:\"kill\" "
        "stops the batch (the turn itself keeps running). Collect or kill a job before the turn ends: the "
        "results are the only thing that outlives it. Returns JSON: "
        "results[{task_index,status,summary,api_calls,duration_seconds,exit_reason,tokens}], total_duration_seconds."));
    obj_set(schema, "parameters", params);
    return schema;
}

std::function<std::string(const Json&)> make_delegate_tool(const DelegateToolDeps& deps) {
    return [deps](const Json& args) -> std::string {
        if (!deps.parent) return tool_error("delegate_task requires a parent agent context.");
        const int depth = deps.parent->delegate_depth;
        if (depth >= kDelegateMaxDepth)
            return tool_error("Delegation depth limit reached (" + std::to_string(kDelegateMaxDepth) +
                              "). Subagents cannot spawn further subagents.");

        int max_iterations = kDelegateMaxIterations;
        if (const Json* mi = json_get(args, "max_iterations"); mi && mi->type == Json::Num && mi->num > 0)
            max_iterations = static_cast<int>(mi->num);

        // Which toolsets the children keep: requested ∩ what a child has, with
        // "delegation" stripped (delegate_tool.py::_strip_blocked_tools); the
        // depth rule above decides delegate_task on its own.
        std::set<std::string> wanted;
        if (const Json* ts = json_get(args, "toolsets"); ts && ts->type == Json::Arr)
            for (const auto& sp : ts->arr)
                if (sp && sp->type == Json::Str && sp->str != "delegation") wanted.insert(sp->str);

        const ParentFacts facts = facts_of(deps);
        const int child_depth = facts.depth + 1;
        const std::string action = str_arg(args, "action");
        const Json* bg_flag = json_get(args, "background");
        const bool background = bg_flag && bg_flag->type == Json::Bool && bg_flag->b;

        // ---- the job verbs: a batch that outlives this call ----
        // `start` returns while the children run; the turn keeps working. Everything else looks at
        // a job by id. `collect` is where a blocking join belongs — by then the caller has said it
        // is ready to wait.
        const int job_arg = [&] {
            for (const char* k : {"job_id", "job"}) {
                if (const Json* v = json_get(args, k); v && v->type == Json::Num) return static_cast<int>(v->num);
            }
            return 0;
        }();

        if (action == "list") {
            Json arr = json_arr();
            for (auto& j : pool().jobs) {
                const int st = j.state.load(std::memory_order_acquire);
                if (st == kJobFree) continue;
                arr_push(arr, job_view(j));
            }
            return json_dump(json_obj({{"jobs", jsp(std::move(arr))},
                                       {"capacity", jsp(json_num(static_cast<double>(kDelegateMaxJobs)))}}));
        }

        if (action == "status" || action == "wait" || action == "kill" || action == "collect") {
            Job* j = job_arg > 0 ? find_job(job_arg) : nullptr;
            if (!j) return tool_error("no such delegate job" + (job_arg > 0 ? " " + std::to_string(job_arg) : std::string()) +
                                      "; delegate_task(action:\"list\") names the live ones");
            if (action == "kill") {
                // A finished job is not killed: its children already answered, and relabelling
                // completed work as "killed" would be a lie in the log and in the results.
                const bool live = j->state.load(std::memory_order_acquire) != kJobFinished;
                if (live) j->killed.store(true, std::memory_order_release);
                j->stop.store(true, std::memory_order_release);   // wire calls and the child's loop
                // A kill is a signal, not a reap: joining here would block this call for however
                // long a child's tool takes to notice — the opposite of what the caller asked for.
                // The children stop on the flag (wire ~ms, shell at its poll, loop between steps);
                // `collect` is where the join belongs, and the slot is not reusable until then.
                Json o = job_view(*j);
                if (!live) {
                    obj_set(o, "note", json_str("already done — nothing to kill; collect it for the results"));
                } else {
                    obj_set(o, "note", json_str("killed: children that had finished keep their results; "
                                                "delegate_task(action:\"collect\", job_id:" +
                                                std::to_string(job_arg) + ") waits for the rest to stop and reads them"));
                }
                return json_dump(o);
            }
            if (action == "wait") {
                // A wait is the caller saying it wants to block. It waits up to timeout_ms (default
                // 60 s, ceiling 10 min) and then answers honestly with whatever state it found.
                long long timeout_ms = 60000;
                if (const Json* t = json_get(args, "timeout_ms"); t && t->type == Json::Num && t->num >= 0)
                    timeout_ms = std::min<long long>(static_cast<long long>(t->num), 600000);
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
                while (j->state.load(std::memory_order_acquire) != kJobFinished &&
                       std::chrono::steady_clock::now() < deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                if (j->state.load(std::memory_order_acquire) != kJobFinished) {
                    Json o = job_view(*j);
                    obj_set(o, "note", json_str("still running after " + std::to_string(timeout_ms) +
                                                " ms; ask again or collect it"));
                    return json_dump(o);
                }
            }
            if (action == "collect" || action == "wait" || action == "status") {
                if (action != "status") join_job(*j);   // finished by now: returns at once
                const int st = j->state.load(std::memory_order_acquire);
                if (action == "status" && st != kJobFinished) return json_dump(job_view(*j));
                Json arr = json_arr();
                const int n = j->n.load(std::memory_order_relaxed);
                for (int i = 0; i < n; ++i) {
                    if (j->child_state[static_cast<size_t>(i)].load(std::memory_order_acquire) == 2)
                        arr_push(arr, j->results[static_cast<size_t>(i)]);
                }
                Json out = json_obj({{"job_id", jsp(json_num(job_arg))},
                                     {"state", jsp(json_str(state_word(*j)))},
                                     {"results", jsp(std::move(arr))}});
                obj_set(out, "total_duration_seconds",
                        json_num(static_cast<double>(j->ended_ms.load(std::memory_order_acquire) -
                                                     j->started_ms.load(std::memory_order_relaxed)) / 1000.0));
                if (action == "collect" || (action == "wait" && st == kJobFinished)) {
                    // Retire: the slot is free for the next batch. The result cells are copied out;
                    // the next start overwrites them, and no reader may touch them after this store.
                    j->id.store(0, std::memory_order_relaxed);
                    j->n.store(0, std::memory_order_relaxed);
                    j->done.store(0, std::memory_order_relaxed);
                    j->killed.store(false, std::memory_order_relaxed);
                    j->state.store(kJobFree, std::memory_order_release);
                }
                return json_dump(out);
            }
        }

        // Task list: batch (capped) or single.
        struct Task { std::string goal, context; };
        std::vector<Task> tasks;
        if (const Json* tl = json_get(args, "tasks"); tl && tl->type == Json::Arr && !tl->arr.empty()) {
            for (const auto& sp : tl->arr) {
                if (!sp || sp->type != Json::Obj) return tool_error("each task must be an object with a 'goal'.");
                Task t{str_arg(*sp, "goal"), str_arg(*sp, "context")};
                const Json* sp_ts = json_get(*sp, "toolsets");
                if (sp_ts && sp_ts->type == Json::Arr)
                    for (const auto& s2 : sp_ts->arr)
                        if (s2 && s2->type == Json::Str && s2->str != "delegation") wanted.insert(s2->str);
                tasks.push_back(std::move(t));
            }
            if (tasks.size() > static_cast<size_t>(kDelegateMaxChildren))
                tasks.resize(static_cast<size_t>(kDelegateMaxChildren));
        } else {
            const std::string goal = str_arg(args, "goal");
            if (goal.empty()) return tool_error("Provide either 'goal' (single task) or 'tasks' (batch).");
            tasks.push_back({goal, str_arg(args, "context")});
        }
        for (size_t i = 0; i < tasks.size(); ++i)
            if (tasks[i].goal.empty())
                return tool_error("Task " + std::to_string(i) + " is missing a 'goal'.");

        // ---- start: the batch runs in the background and this call returns at once ----
        if (action == "start" || ((action.empty() || action == "run") && background)) {
            Job* j = take_slot();
            if (!j) return tool_error("delegate depot full (" + std::to_string(kDelegateMaxJobs) +
                                      " jobs); collect or kill one: delegate_task(action:\"list\")");
            const int n = static_cast<int>(tasks.size());
            j->id.store(pool().next_id.fetch_add(1, std::memory_order_relaxed), std::memory_order_relaxed);
            j->n.store(n, std::memory_order_relaxed);
            j->done.store(0, std::memory_order_relaxed);
            j->stop.store(false, std::memory_order_relaxed);
            j->killed.store(false, std::memory_order_relaxed);
            j->joined.store(false, std::memory_order_release);
            j->started_ms.store(now_ms(), std::memory_order_relaxed);
            j->ended_ms.store(0, std::memory_order_relaxed);
            for (int i = 0; i < kDelegateMaxChildren; ++i) {
                j->results[static_cast<size_t>(i)] = json_obj();
                j->child_state[static_cast<size_t>(i)].store(0, std::memory_order_relaxed);
                j->goals[static_cast<size_t>(i)] =
                    i < n ? tasks[static_cast<size_t>(i)].goal : std::string();
            }
            // The wire by value, and a deps copy with no parent: nothing a worker reads points into
            // the turn that started it.
            j->spec = deps;
            j->spec.parent = nullptr;
            if (deps.transport) {
                j->wire.emplace(*deps.transport);
                j->spec.transport = &*j->wire;
            } else {
                j->wire.reset();
                j->spec.transport = nullptr;
            }
            j->facts = facts;
            j->toolsets = wanted;

            std::vector<std::shared_ptr<agent::LLMClient>> assigned;
            if (deps.llm_factory) {
                assigned.reserve(tasks.size());
                for (size_t i = 0; i < tasks.size(); ++i) assigned.push_back(deps.llm_factory());
            }
            const int job_id = j->id.load(std::memory_order_relaxed);
            for (int i = 0; i < n; ++i) {
                const std::string goal = tasks[static_cast<size_t>(i)].goal;
                const std::string context = tasks[static_cast<size_t>(i)].context;
                auto sp = assigned.empty() ? nullptr : assigned[static_cast<size_t>(i)];
                j->workers.emplace_back([j, i, goal, context, sp, max_iterations] {
                    j->child_state[static_cast<size_t>(i)].store(1, std::memory_order_release);
                    Json r = run_one_child(i, goal, context, j->facts, max_iterations, j->spec,
                                           j->toolsets, sp, &j->stop);
                    j->results[static_cast<size_t>(i)] = std::move(r);
                    j->child_state[static_cast<size_t>(i)].store(2, std::memory_order_release);
                    if (j->done.fetch_add(1, std::memory_order_acq_rel) + 1 ==
                        j->n.load(std::memory_order_relaxed)) {
                        j->ended_ms.store(now_ms(), std::memory_order_release);
                        j->state.store(kJobFinished, std::memory_order_release);
                    }
                });
            }
            j->state.store(kJobRunning, std::memory_order_release);
            {
                using namespace dd::core;
                logbook::emit(logbook::Level::Info, "delegate", "batch.start",
                              {{"job_id", logbook::N(static_cast<double>(job_id))},
                               {"n", logbook::N(static_cast<double>(n))},
                               {"parallel", logbook::B(n > 1)},
                               {"depth", logbook::N(static_cast<double>(child_depth))},
                               {"background", logbook::B(true)}});
            }
            Json o = job_view(*j);
            obj_set(o, "note", json_str("started in the background: this call returns while the children run. "
                                        "Poll with delegate_task(action:\"status\", job_id:" + std::to_string(job_id) +
                                        "), collect with action:\"collect\", cancel with action:\"kill\"."));
            return json_dump(o);
        }

        const auto started = std::chrono::steady_clock::now();
        std::vector<Json> results(tasks.size());
        // One transport per child, assigned here — on the handler's thread, before
        // any child thread starts. A factory that is itself thread-unsafe cannot be
        // raced by this tool; the pool hands out, the threads only run.
        std::vector<std::shared_ptr<agent::LLMClient>> assigned;
        if (deps.llm_factory) {
            assigned.reserve(tasks.size());
            for (size_t i = 0; i < tasks.size(); ++i) assigned.push_back(deps.llm_factory());
        }

        if (tasks.size() == 1) {
            results[0] = run_one_child(0, tasks[0].goal, tasks[0].context, facts,
                                       max_iterations, deps, wanted,
                                       assigned.empty() ? nullptr : assigned[0], nullptr);
        } else {
            std::vector<std::thread> workers;
            workers.reserve(tasks.size());
            for (size_t i = 0; i < tasks.size(); ++i)
                workers.emplace_back([&, i] {
                    results[i] = run_one_child(static_cast<int>(i), tasks[i].goal, tasks[i].context,
                                               facts, max_iterations, deps, wanted,
                                               assigned.empty() ? nullptr : assigned[i], nullptr);
                });
            for (auto& t : workers) t.join();
        }
        const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        // The batch as one fact: how many ran, whether they ran at once, and the wall time against
        // the sum of the children's own times. `wall_ms` well under `sum_ms` is the proof that the
        // children overlapped — the number to check before believing a "parallel" claim.
        {
            using namespace dd::core;
            double sum_ms = 0.0;
            for (const auto& r : results)
                if (const Json* d = json_get(r, "duration_seconds"); d && d->type == Json::Num) sum_ms += d->num * 1000.0;
            logbook::emit(logbook::Level::Info, "delegate", "batch",
                          {{"n", logbook::N(static_cast<double>(tasks.size()))},
                           {"parallel", logbook::B(tasks.size() > 1)},
                           {"depth", logbook::N(static_cast<double>(child_depth))},
                           {"background", logbook::B(false)},
                           {"wall_ms", logbook::N(total * 1000.0)},
                           {"sum_ms", logbook::N(sum_ms)}});
        }

        Json arr = json_arr();
        for (auto& r : results) arr_push(arr, r);
        Json out = json_obj({{"results", jsp(std::move(arr))},
                             {"total_duration_seconds", jsp(json_num(std::round(total * 100.0) / 100.0))}});
        return json_dump(out);
    };
}

}  // namespace tools
}  // namespace dd
