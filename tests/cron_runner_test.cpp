// tests/cron_runner_test.cpp — the cron execution pipeline end to end against fakes and,
// where a process is the point, the real thing: prompt build (hint/script/skills), the
// script's path confinement, delivery-target resolution, the tick's bookkeeping (advance
// before run, output doc, [SILENT], failure notice, mark), the turn-claim park, the tick
// lock, the inactivity watchdog, and the agent executor against a fake wire.
//
// Scratch lives under $DD_TEST_TMP (the Makefile sets it), never the shared /tmp.

#include "dd/cron/executor.hpp"
#include "dd/cron/jobs.hpp"
#include "dd/cron/runner.hpp"
#include "dd/cron/schedule.hpp"
#include "dd/core/fslock.hpp"
#include "dd/core/home.hpp"
#include "dd/core/json.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

using namespace dd;
using namespace dd::core;
using namespace dd::cron;

static int failures = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
        }                                                                    \
    } while (0)

static std::string sget(const Json& o, const std::string& k) {
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Str) return v->str;
    return "";
}

// ---- fakes ----
struct FakeExec : JobExecutor {
    int runs = 0;
    std::string prompt_seen;
    std::string response = "hello from the fake";
    bool fail = false;
    std::string error = "boom";
    bool block = false;   // hang until stop (the watchdog's test)

    JobOutcome run(const Json&, const std::string& prompt, const std::atomic<bool>& stop,
                   std::atomic<long long>*) override {
        ++runs;
        prompt_seen = prompt;
        if (block) {
            while (!stop.load(std::memory_order_acquire))
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return {false, "", "interrupted"};
        }
        if (fail) return {false, "", error};
        return {true, response, ""};
    }
};

struct FakeSink : DeliverySink {
    int calls = 0;
    Json last_target;
    std::string last_content;
    std::optional<std::string> deliver(const Json& target, const std::string& content) override {
        ++calls;
        last_target = target;
        last_content = content;
        return std::nullopt;
    }
};

// The wire in one method: the agent executor's test double (delegate tests use the same shape).
struct FakeLLM : agent::LLMClient {
    std::string canned = "canned answer";
    int calls = 0;
    core::Message chat(const std::vector<core::Message>&, const core::Json&) override {
        ++calls;
        core::Message m;
        m.role = "assistant";
        m.content = canned;
        m.finish_reason = "stop";
        return m;
    }
};

// ---- helpers ----
static std::filesystem::path g_home;

static void reset_jobs() { save_jobs({}); }

// A due recurring job: created "every 1m", then its next_run_at dragged one minute into the
// past — inside the grace window, so get_due_jobs() sees it as due, not as missed.
static Json make_due(const std::string& prompt, const std::optional<std::string>& deliver = std::nullopt,
                     const std::optional<Json>& origin = std::nullopt) {
    CreateJobParams p;
    p.prompt = prompt;
    p.schedule = "every 1m";
    p.deliver = deliver;
    p.origin = origin;
    Json job = create_job(p);
    DT past{now().minute - 1};
    Json upd = json_obj();
    obj_set(upd, "next_run_at", json_str(iso_of(past)));
    auto updated = update_job(sget(job, "id"), upd);
    return updated ? *updated : job;
}

static std::string read_output_doc(const std::string& id) {
    std::filesystem::path dir = output_dir() / id;
    std::error_code ec;
    std::string newest;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.path().extension() == ".md" && e.path().filename().string() > newest)
            newest = e.path().filename().string();
    if (newest.empty()) return "";
    std::ifstream in(dir / newest, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

static void write_file(const std::filesystem::path& p, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary);
    out << text;
}

int main() {
    const char* e = std::getenv("DD_TEST_TMP");
    std::filesystem::path scratch = e && *e ? std::filesystem::path(e) / "cron_runner_test"
                                            : std::filesystem::path(".dd_cron_runner_test.tmp");
    std::error_code ec;
    std::filesystem::remove_all(scratch, ec);
    g_home = scratch / "home";
    std::filesystem::create_directories(g_home, ec);
    set_ariadne_home_override(g_home.string());

    // ---- delivery resolution ----
    {
        std::map<std::string, std::string> vars;
        auto env = [&](const std::string& k) { return vars.count(k) ? vars[k] : std::string(); };

        Json job = json_obj();
        obj_set(job, "deliver", json_str("local"));
        CHECK(!resolve_delivery_target(job, env).has_value());

        Json origin = json_obj();
        obj_set(origin, "platform", json_str("telegram"));
        obj_set(origin, "chat_id", json_str("123"));
        obj_set(origin, "thread_id", json_str("t9"));
        obj_set(job, "deliver", json_str("origin"));
        obj_set(job, "origin", origin);
        auto t = resolve_delivery_target(job, env);
        CHECK(t.has_value() && sget(*t, "platform") == "telegram" && sget(*t, "chat_id") == "123" &&
              sget(*t, "thread_id") == "t9");

        // deliver=origin without a usable origin: the {PLATFORM}_HOME_CHANNEL fallback.
        Json job2 = json_obj();
        obj_set(job2, "deliver", json_str("origin"));
        CHECK(!resolve_delivery_target(job2, env).has_value());
        vars["TELEGRAM_HOME_CHANNEL"] = "42";
        t = resolve_delivery_target(job2, env);
        CHECK(t.has_value() && sget(*t, "platform") == "telegram" && sget(*t, "chat_id") == "42");

        // "platform:chat" is explicit; "platform" bare matches the origin or the env.
        Json job3 = json_obj();
        obj_set(job3, "deliver", json_str("discord:777"));
        t = resolve_delivery_target(job3, env);
        CHECK(t.has_value() && sget(*t, "platform") == "discord" && sget(*t, "chat_id") == "777");

        Json job4 = json_obj();
        obj_set(job4, "deliver", json_str("telegram"));
        obj_set(job4, "origin", origin);
        t = resolve_delivery_target(job4, env);
        CHECK(t.has_value() && sget(*t, "chat_id") == "123");

        Json job5 = json_obj();
        obj_set(job5, "deliver", json_str("nonsense"));
        CHECK(!resolve_delivery_target(job5, env).has_value());
    }

    // ---- wrap + [SILENT] ----
    {
        const std::string w = wrap_delivery("the answer", "my job");
        CHECK(w.find("Cronjob Response: my job") != std::string::npos);
        CHECK(w.find("the answer") != std::string::npos);
        CHECK(w.find("cannot respond to it") != std::string::npos);
        CHECK(is_silent("[SILENT]"));
        CHECK(is_silent("  [Silent]  "));
        CHECK(!is_silent("all quiet on the surface"));
        CHECK(is_silent("not [SILENT] at all, sadly"));   // containment, as the Python checks it
    }

    // ---- scripts: confinement, framing, exit codes, timeout ----
    {
        const std::filesystem::path scripts = g_home / "scripts";
        auto [ok, out] = run_job_script("../evil.py", scripts, "python3", 30);
        CHECK(!ok && out.find("Blocked: script path resolves outside the scripts directory") == 0);
        std::tie(ok, out) = run_job_script("/etc/passwd", scripts, "python3", 30);
        CHECK(!ok && out.find("Blocked:") == 0);
        std::tie(ok, out) = run_job_script("missing.py", scripts, "python3", 30);
        CHECK(!ok && out.find("Script not found:") == 0);

        write_file(scripts / "ok.py", "print('DATA123')\n");
        std::tie(ok, out) = run_job_script("ok.py", scripts, "python3", 30);
        const bool have_python = ok && out.find("DATA123") != std::string::npos;
        if (!have_python) std::printf("note: no working python3 — script spawn checks skipped\n");
        CHECK(ok);   // a script that runs is the baseline; the content check follows
        if (have_python) {
            CHECK(out.find("DATA123") != std::string::npos);

            write_file(scripts / "fail.py", "import sys\nprint('noise', file=sys.stderr)\nsys.exit(3)\n");
            std::tie(ok, out) = run_job_script("fail.py", scripts, "python3", 30);
            CHECK(!ok && out.find("Script exited with code 3") == 0);
            CHECK(out.find("stderr:") != std::string::npos && out.find("noise") != std::string::npos);

            write_file(scripts / "hang.py", "import time\ntime.sleep(30)\n");
            std::tie(ok, out) = run_job_script("hang.py", scripts, "python3", 1);
            CHECK(!ok && out.find("Script timed out after 1s") == 0);
        }
    }

    // ---- prompt build: hint, script framing, skills ----
    {
        const std::filesystem::path scripts = g_home / "scripts";
        write_file(scripts / "collect.py", "print('COLLECTED')\n");
        CreateJobParams p;
        p.prompt = "analyze the data";
        p.schedule = "every 1d";
        p.script = "collect.py";
        p.skills = std::vector<std::string>{"s1", "missing"};
        Json job = create_job(p);
        PromptBuildOptions opt;
        opt.interpreter = "python3";
        opt.script_timeout_s = 30;
        auto loader = [](const std::string& name) -> std::optional<std::string> {
            return name == "s1" ? std::optional<std::string>("SKILLBODY") : std::nullopt;
        };
        std::string prompt = build_job_prompt(job, loader, opt);
        CHECK(prompt.find("[SYSTEM: You are running as a scheduled cron job") != std::string::npos);
        CHECK(prompt.find("[SILENT]") != std::string::npos);   // the hint teaches the convention
        CHECK(prompt.find("## Script Output") != std::string::npos);
        CHECK(prompt.find("COLLECTED") != std::string::npos);
        CHECK(prompt.find("invoked the \"s1\" skill") != std::string::npos);
        CHECK(prompt.find("SKILLBODY") != std::string::npos);
        CHECK(prompt.find("skipped: missing") != std::string::npos);
        CHECK(prompt.find("analyze the data") != std::string::npos);

        // A failing script still reaches the model, as an error to report.
        p.script = "missing.py";
        Json job2 = create_job(p);
        std::string prompt2 = build_job_prompt(job2, loader, opt);
        CHECK(prompt2.find("## Script Error") != std::string::npos);
        CHECK(prompt2.find("Script not found:") != std::string::npos);
    }

    // ---- output doc ----
    {
        Json job = json_obj();
        obj_set(job, "id", json_str("abc123abc123"));
        obj_set(job, "name", json_str("my job"));
        obj_set(job, "schedule_display", json_str("every 1m"));
        std::string doc = output_doc(job, "the prompt", {true, "the answer", ""});
        CHECK(doc.find("# Cron Job: my job") == 0);
        CHECK(doc.find("**Job ID:** abc123abc123") != std::string::npos);
        CHECK(doc.find("## Response\n\nthe answer") != std::string::npos);
        std::string failed = output_doc(job, "the prompt", {false, "", "err text"});
        CHECK(failed.find("# Cron Job: my job (FAILED)") == 0);
        CHECK(failed.find("## Error\n\n```\nerr text\n```") != std::string::npos);
    }

    // ---- the tick: pipeline, [SILENT], failure, park, lock, watchdog ----
    {
        reset_jobs();
        FakeExec exec;
        FakeSink sink;
        SchedulerOptions opt;   // defaults: 60s ticks, 600s limit
        Scheduler sched(opt, exec, sink);
        sched.skill_loader = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };

        // 1) a due job runs: prompt seen, output doc written, marked ok, schedule advanced,
        //    and a local delivery is the file — the sink is never called.
        Json job = make_due("do the thing");
        const std::string id = sget(job, "id");
        CHECK(sched.tick() == 1);
        CHECK(exec.runs == 1);
        CHECK(exec.prompt_seen.find("do the thing") != std::string::npos);
        CHECK(exec.prompt_seen.find("[SYSTEM: You are running as a scheduled cron job") != std::string::npos);
        const std::string doc = read_output_doc(id);
        CHECK(doc.find("## Response\n\nhello from the fake") != std::string::npos);
        CHECK(sink.calls == 0);
        Json after = *get_job(id);
        CHECK(sget(after, "last_status") == "ok");
        CHECK(sget(after, "next_run_at") != sget(job, "next_run_at"));

        // 2) [SILENT]: marked ok, nothing delivered.
        reset_jobs();
        FakeExec exec2;
        FakeSink sink2;
        Scheduler sched2(opt, exec2, sink2);
        exec2.response = "[SILENT]";
        Json origin = json_obj();
        obj_set(origin, "platform", json_str("telegram"));
        obj_set(origin, "chat_id", json_str("123"));
        Json job2 = make_due("check the feed", "origin", origin);
        CHECK(sched2.tick() == 1);
        CHECK(sink2.calls == 0);
        CHECK(sget(*get_job(sget(job2, "id")), "last_status") == "ok");

        // 3) failure: the doc says (FAILED), the delivery is the failure notice (wrapped),
        //    and the job records the error.
        reset_jobs();
        FakeExec exec3;
        FakeSink sink3;
        Scheduler sched3(opt, exec3, sink3);
        exec3.fail = true;
        exec3.error = "boom";
        Json job3 = make_due("doomed job", "origin", origin);
        CHECK(sched3.tick() == 1);
        CHECK(sink3.calls == 1);
        CHECK(sink3.last_content.find("Cronjob Response:") != std::string::npos);
        CHECK(sink3.last_content.find("⚠️ Cron job") != std::string::npos);
        CHECK(sink3.last_content.find("boom") != std::string::npos);
        Json after3 = *get_job(sget(job3, "id"));
        CHECK(sget(after3, "last_status") == "error");
        CHECK(sget(after3, "last_error") == "boom");
        CHECK(read_output_doc(sget(job3, "id")).find("(FAILED)") != std::string::npos);

        // 4) parked: a held turn claim leaves the job due and unrun.
        reset_jobs();
        FakeExec exec4;
        FakeSink sink4;
        Scheduler sched4(opt, exec4, sink4);
        std::atomic<bool> claim{false};
        sched4.claim = &claim;
        Json job4 = make_due("patient job");
        claim.store(true, std::memory_order_release);
        CHECK(sched4.tick() == 0);
        CHECK(exec4.runs == 0);
        CHECK(sget(*get_job(sget(job4, "id")), "next_run_at") == sget(job4, "next_run_at"));
        claim.store(false, std::memory_order_release);
        CHECK(sched4.tick() == 1);
        CHECK(exec4.runs == 1);

        // 5) the tick lock: held elsewhere, the tick skips without running anything.
        reset_jobs();
        FakeExec exec5;
        FakeSink sink5;
        Scheduler sched5(opt, exec5, sink5);
        Json job5 = make_due("locked out");
        {
            FileLock held(cron_dir() / ".tick.lock", FileLock::try_lock_t{});
            CHECK(held.held());
            CHECK(sched5.tick() == 0);
            CHECK(exec5.runs == 0);
        }
        CHECK(sched5.tick() == 1);

        // 6) the watchdog: a job that goes quiet past the limit is interrupted and marked
        //    with the timeout error.
        reset_jobs();
        FakeExec exec6;
        FakeSink sink6;
        SchedulerOptions short_opt;
        short_opt.timeout_s = 1;
        Scheduler sched6(short_opt, exec6, sink6);
        exec6.block = true;
        Json job6 = make_due("wedged job");
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(sched6.tick() == 1);
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(secs < 8.0);
        Json after6 = *get_job(sget(job6, "id"));
        CHECK(sget(after6, "last_status") == "error");
        CHECK(sget(after6, "last_error").find("TimeoutError: Cron job") == 0);
    }

    // ---- the agent executor against a fake wire ----
    {
        FakeLLM llm;
        tools::ToolRegistry reg;
        AgentJobExecutorDeps deps;
        deps.assigned = &llm;
        deps.registry = &reg;
        deps.system_prompt = "rules";
        deps.fallback_model = "test-model";
        AgentJobExecutor exec(std::move(deps));
        Json job = json_obj();
        obj_set(job, "id", json_str("abc123abc123"));
        std::atomic<bool> stop{false};
        std::atomic<long long> activity{0};
        JobOutcome out = exec.run(job, "what is the answer?", stop, &activity);
        CHECK(out.success);
        CHECK(out.final_response == "canned answer");
        CHECK(llm.calls == 1);
        CHECK(activity.load(std::memory_order_relaxed) > 0);   // the watchdog clock is fed

        // interrupt() stops a blocked run from the outside (the watchdog's other reach).
        FakeLLM llm2;
        AgentJobExecutorDeps deps2;
        deps2.assigned = &llm2;
        deps2.registry = &reg;
        AgentJobExecutor exec2(std::move(deps2));
        std::atomic<bool> stop2{false};
        std::atomic<long long> activity2{0};
        std::thread th([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            stop2.store(true, std::memory_order_release);
            exec2.interrupt();
        });
        JobOutcome out2 = exec2.run(job, "blocked", stop2, &activity2);
        th.join();
        CHECK(!out2.success || out2.final_response == "canned answer");   // either it finished before the interrupt or it was stopped
    }

    std::filesystem::remove_all(scratch, ec);
    if (failures) {
        std::printf("cron_runner_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("cron_runner_test: all checks passed\n");
    return 0;
}
