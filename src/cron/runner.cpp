#include "dd/cron/runner.hpp"

#include "dd/agent/redact.hpp"
#include "dd/core/fslock.hpp"
#include "dd/core/home.hpp"
#include "dd/cron/jobs.hpp"
#include "dd/cron/schedule.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

extern char** environ;   // posix_spawn's environment (POSIX)

namespace dd {
namespace cron {

using namespace core;

// ---- small helpers ----
static std::string sget(const Json& o, const std::string& k) {
    const Json* v = json_get(o, k);
    if (v && v->type == Json::Str) return v->str;
    return "";
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\n\r");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\n\r");
    return s.substr(a, b - a + 1);
}

static long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Run time stamp for the output doc: "YYYY-MM-DD HH:MM:SS" (UTC; the schedule model is UTC).
static std::string run_time_str() {
    std::string iso = iso_of(now());   // YYYY-MM-DDTHH:MM:SS+00:00
    return iso.substr(0, 10) + " " + iso.substr(11, 8);
}

// ---- delivery resolution (port of scheduler.py::_resolve_delivery_target) ----
static const char* kHomeChannelFallback[] = {"matrix", "telegram", "discord", "slack",
                                             "bluebubbles", nullptr};

static bool known_platform(const std::string& p) {
    static const char* k[] = {"telegram", "discord", "slack", "whatsapp", "signal",
                              "matrix",   "mattermost", "homeassistant", "dingtalk",
                              "feishu",   "wecom", "wecom_callback", "weixin", "sms",
                              "email",    "webhook", "bluebubbles", nullptr};
    std::string lower = p;
    for (auto& c : lower) c = (char)::tolower((unsigned char)c);
    for (const char** s = k; *s; ++s) if (lower == *s) return true;
    return false;
}

std::optional<Json> resolve_delivery_target(
    const Json& job, const std::function<std::string(const std::string&)>& envfn) {
    std::string deliver = sget(job, "deliver");
    if (deliver.empty()) deliver = "local";

    const Json* origin_j = json_get(job, "origin");
    Json origin = (origin_j && origin_j->type == Json::Obj) ? *origin_j : Json::make_null();

    if (deliver == "local") return std::nullopt;

    if (deliver == "origin") {
        if (origin.type == Json::Obj && !sget(origin, "platform").empty() &&
            !sget(origin, "chat_id").empty()) {
            Json t = json_obj();
            obj_set(t, "platform", json_str(sget(origin, "platform")));
            obj_set(t, "chat_id", json_str(sget(origin, "chat_id")));
            const Json* th = json_get(origin, "thread_id");
            obj_set(t, "thread_id", th ? *th : Json::make_null());
            return t;
        }
        for (const char** name = kHomeChannelFallback; *name; ++name) {
            std::string env_key = std::string(*name);   // Python: f"{platform.upper()}_HOME_CHANNEL"
            for (auto& c : env_key) c = (char)::toupper((unsigned char)c);
            std::string chat = envfn(env_key + "_HOME_CHANNEL");
            if (!chat.empty()) {
                Json t = json_obj();
                obj_set(t, "platform", json_str(*name));
                obj_set(t, "chat_id", json_str(chat));
                obj_set(t, "thread_id", Json::make_null());
                return t;
            }
        }
        return std::nullopt;
    }

    if (deliver.find(':') != std::string::npos) {
        std::string platform = deliver.substr(0, deliver.find(':'));
        std::string rest = deliver.substr(deliver.find(':') + 1);
        // The Python re-resolves `rest` through the channel directory and a per-platform
        // target parser; those adapters are not in the tree yet, so `rest` is the chat id.
        Json t = json_obj();
        obj_set(t, "platform", json_str(platform));
        obj_set(t, "chat_id", json_str(rest));
        obj_set(t, "thread_id", Json::make_null());
        return t;
    }

    std::string platform = deliver;
    if (origin.type == Json::Obj && sget(origin, "platform") == platform) {
        Json t = json_obj();
        obj_set(t, "platform", json_str(platform));
        obj_set(t, "chat_id", json_str(sget(origin, "chat_id")));
        const Json* th = json_get(origin, "thread_id");
        obj_set(t, "thread_id", th ? *th : Json::make_null());
        return t;
    }
    if (!known_platform(platform)) return std::nullopt;
    std::string upper = platform;
    for (auto& c : upper) c = (char)::toupper((unsigned char)c);
    std::string chat = envfn(upper + "_HOME_CHANNEL");
    if (chat.empty()) return std::nullopt;
    Json t = json_obj();
    obj_set(t, "platform", json_str(platform));
    obj_set(t, "chat_id", json_str(chat));
    obj_set(t, "thread_id", Json::make_null());
    return t;
}

std::string wrap_delivery(const std::string& content, const std::string& task_name) {
    return "Cronjob Response: " + task_name + "\n"
           "-------------\n\n" +
           content + "\n\n"
           "Note: The agent cannot see this message, and therefore cannot respond to it.";
}

bool is_silent(const std::string& final_response) {
    std::string up = trim(final_response);
    for (auto& c : up) c = (char)::toupper((unsigned char)c);
    return up.find("[SILENT]") != std::string::npos;
}

std::optional<std::string> NullDelivery::deliver(const Json& target, const std::string&) {
    return "platform '" + sget(target, "platform") + "' not configured/enabled";
}

// ---- pre-run script (port of scheduler.py::_run_job_script) ----
static std::string expand_user(const std::string& p) {
    if (p == "~" || p.rfind("~/", 0) == 0) {
        const char* home = ::getenv("HOME");
        if (home && *home) return std::string(home) + p.substr(1);
    }
    return p;
}

// Capture with a deadline: stdout and stderr apart, the whole process group killed at the
// deadline (the Python ran [interpreter, path] with subprocess timeout=). Output is bounded
// at 1 MiB per stream: what goes past is read and dropped — the prompt cannot hold it anyway.
static constexpr size_t kScriptCaptureMax = 1u << 20;

static std::pair<int, std::string> spawn_captured(const std::vector<std::string>& argv,
                                                  const std::filesystem::path& cwd,
                                                  long long timeout_s) {
    int outp[2], errp[2];
    if (::pipe2(outp, O_CLOEXEC) != 0 || ::pipe2(errp, O_CLOEXEC) != 0)
        return {-1, "Script execution failed: pipe2"};

    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, outp[1], 1);
    posix_spawn_file_actions_adddup2(&fa, errp[1], 2);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    pid_t pid = 0;
    const int rc = ::posix_spawnp(&pid, argv[0].c_str(), &fa, &at, args.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(outp[1]);
    ::close(errp[1]);
    if (rc != 0) {
        ::close(outp[0]);
        ::close(errp[0]);
        return {-1, "Script execution failed: " + std::string(::strerror(rc))};
    }

    (void)cwd;   // the script's directory is its own cwd; the interpreter resolves absolutely
    ::fcntl(outp[0], F_SETFL, O_NONBLOCK);
    ::fcntl(errp[0], F_SETFL, O_NONBLOCK);
    std::string out, err;
    bool out_open = true, err_open = true;
    const long long deadline = now_ms() + timeout_s * 1000;
    bool timed_out = false;
    while (out_open || err_open) {
        long long remain = deadline - now_ms();
        if (remain <= 0) { timed_out = true; break; }
        struct pollfd pfd[2] = {{outp[0], POLLIN, 0}, {errp[0], POLLIN, 0}};
        int n = out_open && err_open ? 2 : 1;
        if (!out_open) pfd[0] = pfd[1];
        int wait_ms = (int)std::min<long long>(remain, 250);
        ::poll(pfd, n, wait_ms);
        auto drain = [](int fd, std::string& acc, bool& open) {
            char buf[4096];
            for (;;) {
                ssize_t r = ::read(fd, buf, sizeof buf);
                if (r > 0) {
                    if (acc.size() < kScriptCaptureMax) acc.append(buf, std::min<size_t>((size_t)r, kScriptCaptureMax - acc.size()));
                } else if (r == 0) {
                    open = false;
                    break;
                } else {
                    break;   // EAGAIN: nothing more right now
                }
            }
        };
        if (out_open) drain(outp[0], out, out_open);
        if (err_open) drain(errp[0], err, err_open);
    }
    if (timed_out) {
        ::kill(-pid, SIGKILL);
        ::kill(pid, SIGKILL);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    ::close(outp[0]);
    ::close(errp[0]);
    if (timed_out) return {-2, ""};
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : (WIFSIGNALED(status) ? -WTERMSIG(status) : -1);
    std::string merged_err = err;   // kept apart from out by the caller's framing
    return {code, out + "\x1e" + merged_err};   // record separator: stdout \x1e stderr
}

std::pair<bool, std::string> run_job_script(const std::string& script_path,
                                            const std::filesystem::path& scripts_dir,
                                            const std::string& interpreter,
                                            long long timeout_s) {
    std::error_code ec;
    std::filesystem::create_directories(scripts_dir, ec);
    std::filesystem::path dir = std::filesystem::weakly_canonical(scripts_dir, ec);
    if (ec) dir = scripts_dir;

    std::filesystem::path raw(expand_user(script_path));
    std::filesystem::path path = raw.is_absolute()
                                     ? std::filesystem::weakly_canonical(raw, ec)
                                     : std::filesystem::weakly_canonical(dir / raw, ec);
    if (ec) path = raw.is_absolute() ? raw : dir / raw;

    const std::string dir_s = dir.string();
    const std::string path_s = path.string();
    if (path_s != dir_s && path_s.rfind(dir_s + "/", 0) != 0)
        return {false, "Blocked: script path resolves outside the scripts directory (" +
                           dir_s + "): '" + script_path + "'"};
    if (!std::filesystem::exists(path, ec))
        return {false, "Script not found: " + path_s};
    if (!std::filesystem::is_regular_file(path, ec))
        return {false, "Script path is not a file: " + path_s};

    auto [code, cap] = spawn_captured({interpreter.empty() ? std::string("python3") : interpreter,
                                       path_s},
                                      path.parent_path(), timeout_s);
    if (code == -2) return {false, "Script timed out after " + std::to_string(timeout_s) + "s: " + path_s};
    if (code == -1 && cap.rfind("Script execution failed:", 0) == 0) return {false, cap};

    size_t sep = cap.find('\x1e');
    std::string stdout_s = agent::mask_secret_tokens(sep == std::string::npos ? cap : cap.substr(0, sep),
                                                     agent::SecretPolicy::ToolOutput);
    std::string stderr_s = agent::mask_secret_tokens(sep == std::string::npos ? std::string() : cap.substr(sep + 1),
                                                     agent::SecretPolicy::ToolOutput);
    stdout_s = trim(stdout_s);
    stderr_s = trim(stderr_s);
    if (code != 0) {
        std::string parts = "Script exited with code " + std::to_string(code);
        if (!stderr_s.empty()) parts += "\nstderr:\n" + stderr_s;
        if (!stdout_s.empty()) parts += "\nstdout:\n" + stdout_s;
        return {false, parts};
    }
    return {true, stdout_s};
}

// ---- prompt build (port of scheduler.py::_build_job_prompt) ----
static const char* kCronHint =
    "[SYSTEM: You are running as a scheduled cron job. "
    "DELIVERY: Your final response will be automatically delivered "
    "to the user — do NOT use send_message or try to deliver "
    "the output yourself. Just produce your report/output as your "
    "final response and the system handles the rest. "
    "SILENT: If there is genuinely nothing new to report, respond "
    "with exactly \"[SILENT]\" (nothing else) to suppress delivery. "
    "Never combine [SILENT] with content — either report your "
    "findings normally, or say [SILENT] and nothing more.]\n\n";

std::string build_job_prompt(const Json& job, const SkillLoader& skills,
                             const PromptBuildOptions& opt) {
    std::string prompt = sget(job, "prompt");

    const std::string script_path = sget(job, "script");
    if (!script_path.empty()) {
        auto [ok, output] = run_job_script(
            script_path, get_ariadne_home() / "scripts", opt.interpreter, opt.script_timeout_s);
        if (ok) {
            if (!output.empty())
                prompt = "## Script Output\n"
                         "The following data was collected by a pre-run script. "
                         "Use it as context for your analysis.\n\n```\n" +
                         output + "\n```\n\n" + prompt;
            else
                prompt = "[Script ran successfully but produced no output.]\n\n" + prompt;
        } else {
            prompt = "## Script Error\n"
                     "The data-collection script failed. Report this to the user.\n\n```\n" +
                     output + "\n```\n\n" + prompt;
        }
    }

    prompt = std::string(kCronHint) + prompt;

    std::vector<std::string> skill_names;
    const Json* sks = json_get(job, "skills");
    if (sks && sks->type == Json::Arr) {
        for (const auto& e : sks->arr) {
            std::string name = trim(e && e->type == Json::Str ? e->str : "");
            if (!name.empty()) skill_names.push_back(name);
        }
    }
    if (skill_names.empty()) {
        std::string legacy = trim(sget(job, "skill"));
        if (!legacy.empty()) skill_names.push_back(legacy);
    }
    if (skill_names.empty()) return prompt;

    std::vector<std::string> parts, skipped;
    for (const auto& name : skill_names) {
        std::optional<std::string> body = skills ? skills(name) : std::nullopt;
        if (!body) {
            skipped.push_back(name);
            continue;
        }
        if (!parts.empty()) parts.push_back("");
        parts.push_back("[SYSTEM: The user has invoked the \"" + name +
                        "\" skill, indicating they want you to follow its instructions. "
                        "The full skill content is loaded below.]");
        parts.push_back("");
        parts.push_back(trim(*body));
    }
    if (!skipped.empty()) {
        std::string joined;
        for (size_t i = 0; i < skipped.size(); ++i) joined += (i ? ", " : "") + skipped[i];
        parts.insert(parts.begin(),
                     "[SYSTEM: The following skill(s) were listed for this job but could not "
                     "be found and were skipped: " +
                         joined +
                         ". Start your response with a brief notice so the user is aware, "
                         "e.g.: '⚠️ Skill(s) not found and skipped: " +
                         joined + "']");
    }
    if (!prompt.empty())
        parts.push_back(
            "The user has provided the following instruction alongside the skill "
            "invocation: " + prompt);
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? "\n" : "") + parts[i];
    return out;
}

// ---- output document (port of run_job's template) ----
std::string output_doc(const Json& job, const std::string& prompt, const JobOutcome& outcome) {
    std::string name = sget(job, "name");
    if (name.empty()) name = sget(job, "id");
    std::string display = sget(job, "schedule_display");
    if (display.empty()) display = "N/A";
    std::string head = "# Cron Job: " + name + (outcome.success ? "" : " (FAILED)") + "\n\n" +
                       "**Job ID:** " + sget(job, "id") + "\n" +
                       "**Run Time:** " + run_time_str() + "\n" +
                       "**Schedule:** " + display + "\n\n" +
                       "## Prompt\n\n" + prompt + "\n\n";
    if (outcome.success) {
        std::string logged = outcome.final_response.empty() ? "(No response generated)"
                                                            : outcome.final_response;
        return head + "## Response\n\n" + logged + "\n";
    }
    return head + "## Error\n\n```\n" + outcome.error + "\n```\n";
}

// ---- options (cron.* config, DD_CRON_* env) ----
static long long env_num(const char* name, long long fallback) {
    const char* v = ::getenv(name);
    if (!v || !*v) return fallback;
    try {
        return std::stoll(v);
    } catch (const std::exception&) {
        return fallback;
    }
}

SchedulerOptions options_from(const Config& cfg) {
    SchedulerOptions o;
    o.tick_seconds = std::max<long long>(1, cfg.get_int("cron.tick_seconds") > 0
                                                ? cfg.get_int("cron.tick_seconds")
                                                : 60);
    o.timeout_s = env_num("DD_CRON_TIMEOUT",
                          cfg.get_int("cron.timeout_seconds") > 0 ? cfg.get_int("cron.timeout_seconds")
                                                                  : 600);
    o.script_timeout_s = env_num("DD_CRON_SCRIPT_TIMEOUT",
                                 cfg.get_int("cron.script_timeout_seconds") > 0
                                     ? cfg.get_int("cron.script_timeout_seconds")
                                     : 120);
    o.wrap_response = !cfg.has("cron.wrap_response") || cfg.get_bool("cron.wrap_response");
    o.interpreter = cfg.get_string("cron.script_interpreter");
    if (o.interpreter.empty()) o.interpreter = "python3";
    return o;
}

// ---- the ticker ----
Scheduler::Scheduler(SchedulerOptions opt, JobExecutor& exec, DeliverySink& sink)
    : opt_(std::move(opt)), exec_(exec), sink_(sink) {
    env = [](const std::string& k) -> std::string {
        const char* v = ::getenv(k.c_str());
        return v ? std::string(v) : std::string();
    };
}

int Scheduler::tick(const std::atomic<int>* stop) {
    // The tick lock: one tick at a time across processes and threads (cron/.tick.lock).
    ensure_dirs();   // the lock lives in the cron dir; create it before opening the lock
    FileLock lock(cron_dir() / ".tick.lock", FileLock::try_lock_t{});
    if (!lock.held()) return 0;

    std::vector<Json> due;
    try {
        due = get_due_jobs();
    } catch (const std::exception& e) {
        if (note) note(std::string("cron: due-job scan failed: ") + e.what());
        return 0;
    }
    if (due.empty()) return 0;

    int executed = 0;
    for (const Json& job : due) {
        if (stop && stop->load(std::memory_order_acquire) != 0) break;
        const std::string id = sget(job, "id");

        // One turn at a time: a due job parks (stays exactly as due) while an interactive
        // turn holds the claim, instead of interleaving on a single-sequence endpoint.
        if (claim) {
            bool expected = false;
            if (!claim->compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
                if (note) note("cron: job " + id + " parked — a turn is running");
                continue;
            }
        }

        bool timed_out = false, interrupted = false;
        JobOutcome outcome;
        std::string prompt;
        try {
            advance_next_run(id);   // before the run: a crash mid-run leaves the schedule moved on

            PromptBuildOptions popt;
            popt.interpreter = opt_.interpreter;
            popt.script_timeout_s = opt_.script_timeout_s;
            prompt = build_job_prompt(job, skill_loader, popt);

            std::atomic<bool> job_stop{false};
            std::atomic<long long> activity{now_ms()};
            std::atomic<bool> done{false};
            std::thread worker([&] {
                outcome = exec_.run(job, prompt, job_stop, &activity);
                done.store(true, std::memory_order_release);
            });
            while (!done.load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                if (done.load(std::memory_order_acquire)) break;
                if (stop && stop->load(std::memory_order_acquire) != 0) {
                    job_stop.store(true, std::memory_order_release);
                    exec_.interrupt();
                    interrupted = true;
                    break;
                }
                if (opt_.timeout_s > 0) {
                    const long long idle = (now_ms() - activity.load(std::memory_order_relaxed)) / 1000;
                    if (idle >= opt_.timeout_s) {
                        job_stop.store(true, std::memory_order_release);
                        exec_.interrupt();
                        timed_out = true;
                        break;
                    }
                }
            }
            worker.join();
            if (timed_out) {
                const long long idle = (now_ms() - activity.load(std::memory_order_relaxed)) / 1000;
                outcome.success = false;
                outcome.final_response.clear();
                outcome.error = "TimeoutError: Cron job '" + sget(job, "name") + "' idle for " +
                                std::to_string(idle) + "s (limit " + std::to_string(opt_.timeout_s) +
                                "s)";
            }
        } catch (const std::exception& e) {
            if (claim) claim->store(false, std::memory_order_release);
            mark_job_run(id, false, e.what(), std::nullopt);
            continue;
        }

        if (claim) claim->store(false, std::memory_order_release);   // the endpoint is free again

        // Shutdown mid-run: leave the job unmarked — its schedule was already advanced,
        // which is exactly the bookkeeping a crash leaves behind.
        if (interrupted) break;

        try {
            std::filesystem::path out_file = save_job_output(id, output_doc(job, prompt, outcome));
            if (note) note("cron: output saved to: " + out_file.string());

            // Deliveries: the answer on success, the failure notice otherwise. [SILENT] in
            // a successful answer suppresses the send; a delivery error never fails the run.
            std::string name = sget(job, "name");
            if (name.empty()) name = id;
            std::string deliver_content = outcome.success
                                              ? outcome.final_response
                                              : "⚠️ Cron job '" + name + "' failed:\n" + outcome.error;
            bool should_deliver = !deliver_content.empty();
            if (should_deliver && outcome.success && is_silent(deliver_content)) {
                if (note) note("cron: job " + id + " returned [SILENT] — skipping delivery");
                should_deliver = false;
            }
            std::optional<std::string> delivery_error;
            if (should_deliver) {
                try {
                    delivery_error = deliver(job, deliver_content);
                } catch (const std::exception& e) {
                    delivery_error = std::string(e.what());
                }
            }
            mark_job_run(id, outcome.success,
                         outcome.success ? std::optional<std::string>() : std::optional<std::string>(outcome.error),
                         delivery_error);
            executed += 1;
        } catch (const std::exception& e) {
            mark_job_run(id, false, e.what(), std::nullopt);
        }
    }
    return executed;
}

std::optional<std::string> Scheduler::deliver(const Json& job, const std::string& content) {
    auto target = resolve_delivery_target(job, env);
    if (!target) {
        std::string d = sget(job, "deliver");
        if (d.empty()) d = "local";
        if (d != "local") return "no delivery target resolved for deliver=" + d;
        return std::nullopt;
    }
    std::string name = sget(job, "name");
    if (name.empty()) name = sget(job, "id");
    return sink_.deliver(*target, opt_.wrap_response ? wrap_delivery(content, name) : content);
}

void Scheduler::run(const std::atomic<int>& stop) {
    while (stop.load(std::memory_order_acquire) == 0) {
        tick(&stop);
        for (long long i = 0; i < opt_.tick_seconds * 10 && stop.load(std::memory_order_acquire) == 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

}  // namespace cron
}  // namespace dd
