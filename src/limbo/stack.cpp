// src/limbo/stack.cpp — the local inference stack: processes, launch lines, commands.
// py: daedalus_cli/stack.py.
#include "dd/limbo/stack.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netdb.h>
#include <poll.h>
#include <regex>
#include <sstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "dd/limbo/afe.hpp"
#include "dd/limbo/spawn.hpp"
#include "dd/net/http.hpp"

namespace fs = std::filesystem;

namespace dd {
namespace limbo {

const char* const kServers[3] = {"main", "aux", "vis"};
const char* const kProfiles[4] = {"dense", "a3b", "strata", "limbo"};

// ---- output ---------------------------------------------------------------------------

namespace {
std::string paint(bool on, const char* code, const std::string& s) { return on ? std::string(code) + s + "\033[0m" : s; }
constexpr const char* kGreen = "\033[32m";
constexpr const char* kYellow = "\033[33m";
constexpr const char* kRed = "\033[31m";
constexpr const char* kDim = "\033[2m";
constexpr const char* kHead = "\033[1;36m";
}  // namespace

void Out::ok(const std::string& t, const std::string& d) const {
    if (quiet) return;
    std::cout << "  " << paint(color, kGreen, "✓") << " " << t << (d.empty() ? "" : " " + paint(color, kDim, d)) << "\n";
}
void Out::warn(const std::string& t, const std::string& d) const {
    if (quiet) return;
    std::cout << "  " << paint(color, kYellow, "⚠") << " " << t << (d.empty() ? "" : " " + paint(color, kDim, d)) << "\n";
}
void Out::bad(const std::string& t, const std::string& d) const {
    if (quiet) return;
    std::cout << "  " << paint(color, kRed, "✗") << " " << t << (d.empty() ? "" : " " + paint(color, kDim, d)) << "\n";
}
void Out::dim(const std::string& t) const {
    if (quiet) return;
    std::cout << "  " << paint(color, kDim, "·") << " " << paint(color, kDim, t) << "\n";
}
void Out::note(const std::string& t) const {
    if (quiet) return;
    std::cout << "    " << paint(color, kDim, t) << "\n";
}
void Out::section(const std::string& t) const {
    if (quiet) return;
    std::cout << "\n" << paint(color, kHead, "◆ " + t) << "\n";
}
void Out::line(const std::string& t) const {
    if (quiet) return;
    std::cout << t << "\n";
}

// ---- running other programs -----------------------------------------------------------

std::string which(const std::string& name) {
    if (name.find('/') != std::string::npos) return ::access(name.c_str(), X_OK) == 0 ? name : "";
    const char* path = std::getenv("PATH");
    std::istringstream in(path ? path : "/usr/local/bin:/usr/bin:/bin");
    std::string dir;
    while (std::getline(in, dir, ':')) {
        const std::string cand = (dir.empty() ? "." : dir) + "/" + name;
        if (::access(cand.c_str(), X_OK) == 0) return cand;
    }
    return "";
}

namespace {
std::vector<char*> c_argv(const std::vector<std::string>& argv) {
    std::vector<char*> a;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
    return a;
}
}  // namespace

int run(const std::vector<std::string>& argv, const std::string& cwd) {
    if (argv.empty()) return 127;
    std::cout.flush();
    const pid_t pid = ::fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) ::_exit(127);
        auto a = c_argv(argv);
        ::execvp(a[0], a.data());
        std::fprintf(stderr, "  could not run %s: %s\n", a[0], std::strerror(errno));
        ::_exit(127);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int run_capture(const std::vector<std::string>& argv, std::string& out, int timeout_s) {
    out.clear();
    if (argv.empty()) return -1;
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return -1;
    const pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); return -1; }
    if (pid == 0) {
        ::dup2(fds[1], 1);
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) { ::dup2(devnull, 2); ::dup2(devnull, 0); }
        auto a = c_argv(argv);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    ::close(fds[1]);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    char buf[4096];
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left <= 0) { ::kill(pid, SIGKILL); break; }
        pollfd p{fds[0], POLLIN, 0};
        if (::poll(&p, 1, static_cast<int>(left)) <= 0) continue;
        const ssize_t n = ::read(fds[0], buf, sizeof buf);
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// ---- process state --------------------------------------------------------------------

std::string proc_state(pid_t pid) {
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto close = raw.rfind(')');
    if (close == std::string::npos) return "";
    std::istringstream rest(raw.substr(close + 1));
    std::string state;
    rest >> state;
    return state;
}

bool pid_alive(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) != 0 && errno != EPERM) return false;
    // A dead child lingers as a zombie, and signal 0 still succeeds on one.
    return proc_state(pid).rfind('Z', 0) != 0;
}

std::string probe_addr(const std::string& host) {
    return (host.empty() || host == "0.0.0.0" || host == "::" || host == "[::]") ? "127.0.0.1" : host;
}

// A server bound to 0.0.0.0 was probed there, the net client refused the address, and every
// probe failed: start waited out its timeout on a loaded model, and "never two models" did
// not see a server already on the port.
bool port_up(const std::string& host, int port, long timeout_ms) {
    const net::Response r = net::get("http://" + probe_addr(host) + ":" + std::to_string(port) + "/health", timeout_ms, 4096);
    return r.error.empty() && r.status > 0;
}

bool health_ok(const std::string& host, int port, long timeout_ms) {
    const net::Response r = net::get("http://" + probe_addr(host) + ":" + std::to_string(port) + "/health", timeout_ms, 4096);
    return r.error.empty() && r.status == 200;
}

namespace {
// Anything listening at all: the mazemaker pod speaks MCP and has no health route.
bool socket_up(const std::string& host, int port, int timeout_ms = 3000) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) return false;
    bool up = false;
    for (addrinfo* a = res; a && !up; a = a->ai_next) {
        const int fd = ::socket(a->ai_family, a->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, a->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, a->ai_addr, a->ai_addrlen) == 0) up = true;
        else if (errno == EINPROGRESS) {
            pollfd p{fd, POLLOUT, 0};
            int err = 0;
            socklen_t len = sizeof err;
            up = ::poll(&p, 1, timeout_ms) == 1 && ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
        }
        ::close(fd);
    }
    ::freeaddrinfo(res);
    return up;
}

std::string read_text(const fs::path& p) {
    std::ifstream in(p);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ')) s.pop_back();
    return s;
}

void write_text(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::trunc) << s;
}

bool is_profile(const std::string& s) {
    for (const char* p : kProfiles) if (s == p) return true;
    return false;
}

std::string base(const std::string& p) { return fs::path(p).filename().string(); }

std::atomic<bool> g_interrupted{false};
void on_sigint(int) { g_interrupted.store(true); }

void nap(double seconds) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(static_cast<long>(seconds * 1000));
    while (!g_interrupted.load() && std::chrono::steady_clock::now() < end)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
}  // namespace

int machine_physical_cores() {
    std::ifstream in("/proc/cpuinfo");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const int logical = static_cast<int>(std::thread::hardware_concurrency());
    return physical_cores(text, logical > 0 ? logical : 1);
}

// ---- Stack ----------------------------------------------------------------------------

StackConf Stack::conf(bool create, bool announce) const {
    std::string note;
    StackConf c = load_conf(paths_, create, &note);
    if (announce && !note.empty()) out_.dim(note);
    return c;
}

std::string Stack::pid_file(const std::string& name) const { return (paths_.run() / (name + ".pid")).string(); }
std::string Stack::log_file(const std::string& name) const { return (paths_.logs() / (name + ".log")).string(); }

pid_t Stack::pid_of(const std::string& name) const {
    const std::string s = read_text(pid_file(name));
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return (!s.empty() && end && *end == '\0' && v > 0) ? static_cast<pid_t>(v) : 0;
}

std::string Stack::state_of(const std::string& name) const {
    const pid_t pid = pid_of(name);
    if (!pid_alive(pid)) return "stopped";
    return proc_state(pid).rfind('T', 0) == 0 ? "paused" : "running";
}

bool Stack::start_one(const std::string& name, const Launch& l) const {
    const std::string state = state_of(name);
    if (state != "stopped") { out_.warn(name + " already " + state); return true; }
    if (l.argv.empty()) { out_.bad(name + " has no launch line"); return false; }
    const std::string exe = which(l.argv[0]);
    if (exe.empty()) { out_.bad(name + " failed to launch", "(" + l.argv[0] + " not found)"); return false; }
    std::error_code ec;
    fs::create_directories(paths_.logs(), ec);
    std::string err;
    const int log = open_log_sink(log_file(name), err);
    SpawnStdio io;
    io.out_write = log;
    io.log_fd = log;
    const pid_t pid = spawn_process(exe, l.argv, l.cwd, l.env, io, err);
    if (log >= 0) ::close(log);
    if (pid <= 0) { out_.bad(name + " failed to launch", "(" + err + ")"); return false; }
    write_text(pid_file(name), std::to_string(pid));
    out_.dim(name + " starting…");
    return true;
}

bool Stack::wait_ready(const std::string& name, const std::string& host, int port, int seconds) const {
    for (int waited = 0; waited < seconds && !g_interrupted.load(); waited += 2) {
        // Our own process has to be the one alive: another server on the port answers 200 too.
        const pid_t pid = pid_of(name);
        int status = 0;
        ::waitpid(pid, &status, WNOHANG);
        if (!pid_alive(pid)) { out_.bad(name + " died", "— ariadne limbo logs " + name); return false; }
        if (health_ok(host, port)) { out_.ok(name + " up on " + host + ":" + std::to_string(port)); return true; }
        nap(2.0);
    }
    out_.bad(name + " did not answer within " + std::to_string(seconds) + "s", "— ariadne limbo logs " + name);
    return false;
}

void Stack::stop_one(const std::string& name, double grace_s) const {
    const pid_t pid = pid_of(name);
    std::error_code ec;
    if (!pid_alive(pid)) { fs::remove(pid_file(name), ec); return; }
    // CONT first: a paused server never sees the TERM that follows.
    ::kill(pid, SIGCONT);
    ::kill(pid, SIGTERM);
    int status = 0;
    for (int i = 0; i < static_cast<int>(grace_s * 2); ++i) {
        ::waitpid(pid, &status, WNOHANG);
        if (!pid_alive(pid)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (pid_alive(pid)) {
        ::kill(pid, SIGKILL);
        for (int i = 0; i < 20 && pid_alive(pid); ++i) {
            ::waitpid(pid, &status, WNOHANG);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    fs::remove(pid_file(name), ec);
    out_.ok(name + " stopped");
}

std::string Stack::running_profile() const {
    const std::string s = read_text(paths_.run() / "main.profile");
    return is_profile(s) ? s : "";
}

std::string Stack::default_profile(const StackConf& c) const {
    const std::string s = c.str("MAIN_PROFILE", "dense");
    return is_profile(s) ? s : "dense";
}

std::string Stack::strata_config(const StackConf& c) const {
    const fs::path b = c.str("STRATA_DIR", std::string(std::getenv("HOME") ? std::getenv("HOME") : "") + "/projects/strata-origin-upstream");
    const fs::path cfg = c.str("STRATA_CONFIG", "strata-coder-iq1_m.json");
    return (cfg.is_absolute() ? cfg : b / cfg).string();
}

std::pair<std::string, std::string> Stack::a3b_repo_file(const StackConf& c) const {
    return {c.str("A3B_REPO", afe::get(c, "AFE_REPO")), c.str("A3B_FILE", afe::get(c, "AFE_FILE"))};
}

std::pair<std::string, std::string> Stack::profile_model(const StackConf& c, const std::string& profile) const {
    if (profile == "strata") {
        // the run config names the engine and the model; both must be there
        const std::string cfg = strata_config(c);
        const std::string how = "check STRATA_DIR / STRATA_CONFIG in " + paths_.conf().string();
        const std::string json = read_text(cfg);
        std::smatch m;
        static const std::regex exe_re(R"re("exe"\s*:\s*"([^"]*)")re");
        if (json.empty() || !std::regex_search(json, m, exe_re) || ::access(m[1].str().c_str(), X_OK) != 0) return {"", how};
        return {cfg, how};
    }
    if (profile == "a3b") {
        const auto [repo, file] = a3b_repo_file(c);
        return {model_path(repo, file, paths_), "ariadne limbo setup"};
    }
    if (profile == "limbo")  // limbo-engine serves the main model unless LIMBO_REPO / LIMBO_FILE name another
        return {model_path(c.str("LIMBO_REPO", c.str("MAIN_REPO")), c.str("LIMBO_FILE", c.str("MAIN_FILE")), paths_), "ariadne limbo setup"};
    return {model_path(c.str("MAIN_REPO"), c.str("MAIN_FILE"), paths_), "ariadne limbo setup"};
}

std::string Stack::mmproj(const StackConf& c) const {
    const std::string explicit_ = c.str("MMPROJ");
    std::error_code ec;
    if (!explicit_.empty()) {
        if (fs::is_regular_file(explicit_, ec)) return explicit_;
        // a bare file name in MMPROJ gets one retry through the HF cache
        if (fs::path(explicit_).parent_path().empty()) {
            const std::string r = model_path(c.str("MAIN_REPO"), explicit_, paths_);
            if (!r.empty()) return r;
        }
        return explicit_;
    }
    const std::string named = c.str("MAIN_MMPROJ_FILE");
    return named.empty() ? "" : model_path(c.str("MAIN_REPO"), named, paths_);
}

std::vector<std::string> Stack::main_argv(const StackConf& c, const std::string& model) const {
    const std::string kv_pool = c.str("MAIN_KV_POOL");
    const long slots = c.integer("MAIN_SLOTS", 1);
    // Block KV streaming needs one KV stream: a unified buffer collapses any slot count to one.
    const bool needs_unified = !kv_pool.empty() && slots != 1;
    std::vector<std::string> a = {c.server().string(), "--model", model,
                                  "--host", c.str("MAIN_HOST", "127.0.0.1"),
                                  "--port", std::to_string(c.integer("MAIN_PORT", 8080)),
                                  "--ctx-size", std::to_string(c.integer("MAIN_CTX", 131072))};
    if (!c.str("MAIN_SLOT_CTX").empty()) a.insert(a.end(), {"--kv-unified-per-slot", std::to_string(c.integer("MAIN_SLOT_CTX", 0))});
    if (!kv_pool.empty()) a.insert(a.end(), {"--kv-stream-stage-mib", kv_pool});
    a.insert(a.end(), {"-ctk", c.str("MAIN_CTK", "q8_0"), "-ctv", c.str("MAIN_CTV", "q4_0"), "-fa", "on",
                       "-ngl", std::to_string(c.integer("MAIN_NGL", 99)), "-t", std::to_string(c.integer("MAIN_THREADS", 8)),
                       "-np", std::to_string(slots), "-b", "512", "-ub", "512",
                       "--temp", c.str("MAIN_TEMP", "1.0"), "--top-k", "20", "--min-p", "0.00", "--top-p", "0.95"});
    // --reasoning on pinned enable_thinking; auto lets the template think unless told none.
    const std::string reasoning = c.str("MAIN_REASONING", "auto");
    if (reasoning != "auto") a.insert(a.end(), {"--reasoning", reasoning});
    const long budget = c.integer("MAIN_REASONING_BUDGET", -1);
    if (budget != -1) a.insert(a.end(), {"--reasoning-budget", std::to_string(budget)});
    if (c.str("MAIN_LOAD_MODE", "mlock") != "auto") a.insert(a.end(), {"--load-mode", c.str("MAIN_LOAD_MODE", "mlock")});
    a.insert(a.end(), {"-ctxcp", std::to_string(c.integer("MAIN_CTX_CHECKPOINTS", 8)),
                       "--ctx-checkpoints-total", std::to_string(c.integer("MAIN_CTX_CHECKPOINTS_TOTAL", 6))});
    if (!c.str("MAIN_SEED").empty()) a.insert(a.end(), {"--seed", std::to_string(c.integer("MAIN_SEED", -1))});
    // --slot-save-path: the harness's KV watermarks erase a slot's leftover KV; --metrics feeds watch.
    a.insert(a.end(), {"--cache-ram", std::to_string(c.integer("MAIN_CACHE_RAM", 2048)),
                       "--slot-save-path", c.str("MAIN_SLOT_SAVE_PATH", paths_.slots().string()), "--metrics"});
    if (needs_unified || c.flag("MAIN_KV_UNIFIED", false)) {
        // Under -kvu --cache-idle-slots clears the idle slot's prefix on every task of the other.
        a.insert(a.end(), {"-kvu", "--no-cache-idle-slots"});
    }
    const std::string draft_repo = c.str("MAIN_DRAFT_REPO"), draft_file = c.str("MAIN_DRAFT_FILE");
    const std::string draft = draft_file.empty() ? "" : model_path(draft_repo, draft_file, paths_);
    if (!draft.empty()) {
        const std::string spec = c.str("MAIN_SPEC_TYPE", "draft-dflash,ngram-mod");
        a.insert(a.end(), {"--model-draft", draft, "-ngld", std::to_string(c.integer("MAIN_NGL", 99)),
                           "--spec-type", spec,
                           "--spec-draft-n-max", std::to_string(c.integer("MAIN_DRAFT_N_MAX", 2)),
                           "--spec-draft-n-min", std::to_string(c.integer("MAIN_DRAFT_N_MIN", 0))});
        if (spec.find("ngram-mod") != std::string::npos) {
            a.insert(a.end(), {"--spec-ngram-mod-n-match", std::to_string(c.integer("MAIN_NGRAM_N_MATCH", 16)),
                               "--spec-ngram-mod-n-min", std::to_string(c.integer("MAIN_NGRAM_N_MIN", 16)),
                               "--spec-ngram-mod-n-max", std::to_string(c.integer("MAIN_NGRAM_N_MAX", 31))});
        }
        a.insert(a.end(), {"-ctkd", c.str("MAIN_DRAFT_CTK", "q8_0"), "-ctvd", c.str("MAIN_DRAFT_CTV", "q8_0")});
        out_.dim("speculative: " + spec + " (" + base(draft) + ")");
    } else {
        a.insert(a.end(), {"--spec-type", "draft-mtp,ngram-mod", "--spec-draft-n-max", "2",
                           "--spec-ngram-mod-n-match", "24", "--spec-ngram-mod-n-min", "24",
                           "--spec-ngram-mod-n-max", "32"});
        if (!draft_repo.empty() || !draft_file.empty()) out_.warn("DFlash2 draft model missing", "— run: ariadne limbo setup");
        out_.dim("speculative: MTP + ngram-mod (no draft model configured)");
    }
    const std::string projector = mmproj(c);
    std::error_code ec;
    const bool present = !projector.empty() && fs::is_regular_file(projector, ec);
    if (c.flag("MAIN_VISION", false)) {
        if (present) {
            // --no-mmproj-offload keeps the ~890 MB tower in host RAM beside a 27B at -ngl 99.
            a.insert(a.end(), {"-mm", projector, "--no-mmproj-offload", "--image-min-tokens", "1024", "--image-max-tokens", "2048"});
            out_.dim("vision: " + base(projector));
        } else {
            out_.warn("MAIN_VISION=1 but no projector", "— vision calls will 500");
            out_.note("fetch it with: ariadne limbo setup");
        }
    } else if (present) {
        out_.dim("vision: off (MAIN_VISION=0) — projector present, set MAIN_VISION=1 to use it");
    } else {
        out_.note("vision: off — no projector fetched either (ariadne limbo setup to get one)");
    }
    return a;
}

long a3b_threads(const StackConf& c, int physical) {
    // Not MAIN_THREADS: the dense profile runs on the card and a desktop conf held 1 there,
    // which a3b inherited -- 18 layers of experts decoding on one thread. Measured on a 4060 Ti
    // box: 4/8/12 threads gave the same prefill and decode, 16 fought the GPU.
    if (const long t = c.integer("A3B_THREADS", 0); t > 0) return t;
    return std::clamp(physical / 2, 4, 12);
}

std::vector<std::string> Stack::a3b_argv(const StackConf& c, const std::string& model) const {
    const long slots = c.integer("MAIN_SLOTS", 1);
    std::vector<std::string> a = {
        c.server().string(), "--model", model, "-a", c.str("A3B_ALIAS", "qwen3.6-35b-a3b"),
        "--host", c.str("MAIN_HOST", "127.0.0.1"), "--port", std::to_string(c.integer("MAIN_PORT", 8080)),
        "--ctx-size", std::to_string(c.integer("A3B_CTX", c.integer("MAIN_CTX", 131072))),
        "-ctk", c.str("MAIN_CTK", "q8_0"), "-ctv", c.str("MAIN_CTV", "q4_0"), "-fa", "on",
        "-ngl", std::to_string(c.integer("MAIN_NGL", 99)),
        "--n-cpu-moe", std::to_string(c.integer("A3B_N_CPU_MOE", 0)),
        // offloaded expert layers are CPU work on every token: they run on these threads
        "-t", std::to_string(a3b_threads(c, machine_physical_cores())),
        "-np", std::to_string(slots),
        "-b", std::to_string(c.integer("A3B_BATCH", 2048)), "-ub", std::to_string(c.integer("A3B_UBATCH", 2048)),
        "--temp", "1.0", "--top-k", "20", "--min-p", "0.00", "--top-p", "0.95",
        "--presence-penalty", "0.0", "--repeat-penalty", "1.0",
        "--reasoning", "on", "--reasoning-preserve", "--reasoning-format", "deepseek",
        "--reasoning-budget", std::to_string(c.integer("MAIN_REASONING_BUDGET", 12000)),
        "--jinja", "--cont-batching",
        "--cache-ram", std::to_string(c.integer("MAIN_CACHE_RAM", 2048)), "--metrics",
        // the DFlash2 drafter is trained against the 27B; the MTP GGUF carries its own head
        "--spec-type", "draft-mtp,ngram-mod", "--spec-draft-n-max", "2",
        "--spec-ngram-mod-n-match", "24", "--spec-ngram-mod-n-min", "24", "--spec-ngram-mod-n-max", "32"};
    if (slots != 1 || c.flag("MAIN_KV_UNIFIED", false)) a.insert(a.end(), {"-kvu", "--no-cache-idle-slots"});
    out_.dim("speculative: MTP + ngram-mod (a3b)");
    // A3B_VISION, not MAIN_VISION: a projector belongs to its model.
    if (c.flag("A3B_VISION", false)) {
        const std::string projector = model_path(a3b_repo_file(c).first, c.str("A3B_MMPROJ_FILE", "mmproj-BF16.gguf"), paths_);
        if (!projector.empty()) {
            a.insert(a.end(), {"-mm", projector, "--no-mmproj-offload", "--image-min-tokens", "1024", "--image-max-tokens", "2048"});
            out_.dim("vision: " + base(projector));
        } else {
            out_.warn("A3B_VISION=1 but the a3b projector is missing", "— run: ariadne limbo setup");
        }
    }
    return a;
}

std::vector<std::string> Stack::strata_argv(const StackConf& c, const std::string& cfg) const {
    const fs::path b = c.str("STRATA_DIR").empty() ? fs::path(cfg).parent_path() : fs::path(c.str("STRATA_DIR"));
    const fs::path py = b / ".venv/bin/python";
    std::error_code ec;
    return {fs::exists(py, ec) ? py.string() : "python3", (b / "serve/server.py").string(),
            "--engine", "strata", "--config", cfg, "--port", std::to_string(c.integer("MAIN_PORT", 8080))};
}

// limbo-engine's server: one shared K/V pool for every slot; slot 1 the main, slot 0 the prewarming sidekick (LIMBO_SLOTS, 2).
std::vector<std::string> Stack::limbo_argv(const StackConf& c, const std::string& model) const {
    std::vector<std::string> a = {c.limbo_server().string(), "-m", model,
                                  "--host", c.str("MAIN_HOST", "127.0.0.1"),
                                  "--port", std::to_string(c.integer("MAIN_PORT", 8080)),
                                  "--ctx", std::to_string(c.integer("LIMBO_CTX", c.integer("MAIN_CTX", 65536))),
                                  "--slots", std::to_string(c.integer("LIMBO_SLOTS", 2)),
                                  "--kv", c.str("LIMBO_KV", "auto"), "--spec", c.str("LIMBO_SPEC", "auto"),
                                  "--temperature", c.str("MAIN_TEMP", "1.0"), "--top-k", "20", "--top-p", "0.95", "--min-p", "0"};
    std::istringstream extra(c.str("LIMBO_ARGS"));
    for (std::string w; extra >> w;) a.push_back(w);
    return a;
}

Launch Stack::main_launch(const StackConf& c, const std::string& profile) const {
    Launch l;
    l.model = profile_model(c, profile).first;
    if (l.model.empty()) return l;
    if (profile == "strata") {
        l.argv = strata_argv(c, l.model);
        l.cwd = fs::path(l.argv[1]).parent_path().parent_path().string();
        return l;
    }
    if (profile == "limbo") {
        l.argv = limbo_argv(c, l.model);
        return l;
    }
    // Unified memory turns "does not fit" into a spill paid on every decode step: opt-in.
    if (profile == "a3b") {
        l.argv = a3b_argv(c, l.model);
        if (c.flag("A3B_CUDA_UNIFIED_MEMORY", false)) l.env.push_back({"GGML_CUDA_ENABLE_UNIFIED_MEMORY", "1"});
        return l;
    }
    l.argv = main_argv(c, l.model);
    if (c.flag("MAIN_CUDA_UNIFIED_MEMORY", false)) l.env.push_back({"GGML_CUDA_ENABLE_UNIFIED_MEMORY", "1"});
    return l;
}

// ---- commands -------------------------------------------------------------------------

int Stack::cmd_status(const StackConf* given) const {
    const StackConf c = given ? *given : conf();
    const int main_port = c.host_port("main").second;
    for (const char* n : kServers) {
        const std::string name = n;
        if (!c.enabled(name)) continue;
        const auto [host, port] = c.host_port(name);
        const std::string where = host + ":" + std::to_string(port);
        if (name == "aux" && port == main_port) {
            // aux sharing main's port is slot 0 of the main process, with no pid of its own
            if (state_of("main") == "running") out_.ok("aux  slot 0 of main on " + where);
            else out_.dim("aux  stopped (slot 0 of main)");
            continue;
        }
        const std::string state = state_of(name);
        const std::string pid = std::to_string(pid_of(name));
        std::string label = name;
        if (name == "main" && !running_profile().empty()) label = "main (" + running_profile() + ")";
        if (state == "running") {
            if (port_up(host, port)) out_.ok(label + "  " + where + "  pid " + pid);
            else out_.warn(label + "  pid " + pid, "— process up, port not answering yet");
        } else if (state == "paused") {
            out_.warn(label + "  pid " + pid, "— paused");
        } else if (port_up(host, port)) {
            out_.warn(label + "  " + where + " answering", "— not started by ariadne (no pid file)");
        } else {
            out_.dim(label + "  stopped");
        }
    }
    if (!which("nvidia-smi").empty()) {
        std::string vram;
        if (run_capture({"nvidia-smi", "--query-gpu=memory.used,memory.total", "--format=csv,noheader"}, vram, 10) == 0) {
            while (!vram.empty() && vram.back() == '\n') vram.pop_back();
            if (!vram.empty()) out_.note("VRAM: " + vram);
        }
    }
    return 0;
}

int Stack::cmd_report() const {
    const StackConf c = conf(false, false);
    std::error_code ec;
    out_.section("Inference toolchain");
    for (const char* tool : {"git", "cmake", "curl"}) {
        if (!which(tool).empty()) out_.ok(tool); else out_.bad(tool, "— required");
    }
    if (!which("hf").empty()) out_.ok("hf (huggingface CLI)");
    else out_.dim("hf missing — downloads go through curl into " + paths_.models().string());
    if (!which("nvcc").empty()) out_.ok("nvcc");
    else out_.warn("nvcc missing", "— the CUDA build needs the toolkit; setup builds for the CPU without it");
    if (!which("nvidia-smi").empty()) out_.ok("nvidia-smi"); else out_.warn("no nvidia-smi", "— CPU only");

    out_.section("Inference sources");
    if (fs::is_directory(c.llama_dir() / ".git", ec)) out_.ok("llama.cpp at " + c.llama_dir().string());
    else out_.bad("llama.cpp missing", "— run: ariadne limbo setup");
    if (::access(c.server().c_str(), X_OK) == 0) out_.ok("llama-server built");
    else out_.bad("llama-server not built", "— run: ariadne limbo setup");
    if (::access(c.limbo_server().c_str(), X_OK) == 0) out_.ok("limbo_serve_gpu built", "(" + c.limbo_server().string() + ")");
    else if (default_profile(c) == "limbo") out_.bad("limbo_serve_gpu not built", "— run: ariadne limbo setup");
    else out_.dim("limbo_serve_gpu not built — the limbo profile needs it (ariadne limbo setup)");

    out_.section("Local models");
    const std::string main_model = model_path(c.str("MAIN_REPO"), c.str("MAIN_FILE"), paths_);
    if (!main_model.empty()) out_.ok("main (dense): " + base(main_model), "(" + human_size(main_model) + ")");
    else out_.bad("main model missing", "— run: ariadne limbo setup");
    const std::string a3b = profile_model(c, "a3b").first;
    if (!a3b.empty()) out_.ok("main (a3b):   " + base(a3b), "(" + human_size(a3b) + ") — ariadne limbo start a3b");
    else out_.dim("main (a3b): not downloaded — ariadne limbo setup");
    if (c.enabled("aux")) {
        const std::string aux = model_path(c.str("AUX_REPO"), c.str("AUX_FILE"), paths_);
        if (!aux.empty()) out_.ok("aux:  " + base(aux), "(" + human_size(aux) + ")");
        else out_.bad("aux model missing", "— run: ariadne limbo setup");
    }
    const std::string draft_repo = c.str("MAIN_DRAFT_REPO"), draft_file = c.str("MAIN_DRAFT_FILE");
    if (!draft_repo.empty() || !draft_file.empty()) {
        const std::string d = draft_file.empty() ? "" : model_path(draft_repo, draft_file, paths_);
        if (!d.empty()) out_.ok("draft (DFlash2): " + base(d), "(" + human_size(d) + ")");
        else out_.bad("DFlash2 draft model configured but missing", "— run: ariadne limbo setup");
    } else {
        out_.warn("no DFlash2 draft model configured", "— running MTP + ngram-mod instead");
    }
    const std::string projector = mmproj(c);
    if (projector.empty()) {
        out_.warn("no vision projector", "— image input will 500 ('provide the mmproj')");
    } else if (!fs::is_regular_file(projector, ec)) {
        out_.warn("MMPROJ set but not found", "(" + projector + ")");
    } else if (!c.flag("MAIN_VISION", false)) {
        out_.dim("vision: off (MAIN_VISION=0) — " + base(projector) + " present, not loaded");
    } else {
        // a projector from another model loads, then fails at the first image
        const std::string pdim = gguf_value(projector, "clip.vision.projection_dim");
        const std::string pname = gguf_value(projector, "general.name");
        out_.ok("mmproj: " + base(projector), pdim.empty() ? "" : "— " + (pname.empty() ? "?" : pname) + ", projection_dim=" + pdim);
    }

    out_.section("Memory backend");
    std::string url = std::getenv("MM_WONDERLAND_URL") ? std::getenv("MM_WONDERLAND_URL") : "http://127.0.0.1:8765";
    std::string hp = std::regex_replace(url, std::regex("^https?://"), "");
    hp = hp.substr(0, hp.find('/'));
    const auto colon = hp.find(':');
    const std::string mm_host = colon == std::string::npos ? hp : hp.substr(0, colon);
    const int mm_port = colon == std::string::npos ? 8765 : std::atoi(hp.substr(colon + 1).c_str());
    if (socket_up(mm_host.empty() ? "127.0.0.1" : mm_host, mm_port)) out_.ok("mazemaker listening on " + mm_host + ":" + std::to_string(mm_port));
    else out_.bad("mazemaker NOT reachable at " + url);

    out_.section("Inference servers");
    cmd_status(&c);
    out_.section("mazemaker AFE model");
    afe::cmd_status(*this, &c);
    out_.line("");
    return 0;
}

bool Stack::fetch(const std::string& repo, const std::string& file, const std::string& label) const {
    if (!model_path(repo, file, paths_).empty()) { out_.ok(label + " present"); return true; }
    if (repo.empty() || file.empty()) return false;
    out_.line("  fetching " + label + " (" + file + ")…");
    if (!which("hf").empty()) return run({"hf", "download", repo, file}) == 0 && !model_path(repo, file, paths_).empty();
    // No hf CLI: the same file over HTTPS into <home>/models, resumable.
    const fs::path dst = local_model_file(repo, file, paths_);
    std::error_code ec;
    fs::create_directories(dst.parent_path(), ec);
    const std::string part = dst.string() + ".part";
    const int rc = run({"curl", "-fL", "--retry", "5", "--retry-delay", "5", "-C", "-", "-o", part,
                        "https://huggingface.co/" + repo + "/resolve/main/" + file});
    if (rc != 0) return false;
    fs::rename(part, dst, ec);
    return !ec;
}

int Stack::cmd_setup() const {
    StackConf c = conf();
    const int physical = machine_physical_cores();
    const auto [pm, pa, pv] = propose_threads(physical);
    const long cm = c.integer("MAIN_THREADS", 0), ca = c.integer("AUX_THREADS", 0), cv = c.integer("VIS_THREADS", 0);
    out_.section("thread budget (pure physical cores)");
    out_.line("  hardware       : " + std::to_string(physical) + " physical core(s), "
              + std::to_string(std::thread::hardware_concurrency()) + " logical CPU(s)");
    out_.line("  current conf   : MAIN=" + std::to_string(cm) + " AUX=" + std::to_string(ca) + " VIS=" + std::to_string(cv));
    out_.line("  proposed       : MAIN=" + std::to_string(pm) + " AUX=" + std::to_string(pa) + " VIS=" + std::to_string(pv)
              + " (sum <= " + std::to_string(physical) + ")");
    if (cm != pm || ca != pa || cv != pv) {
        std::ifstream in(paths_.conf());
        std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!text.empty() && patch_threads(text, pm, pa, pv)) {
            std::ofstream(paths_.conf(), std::ios::trunc) << text;
            c = conf(false, false);
            out_.ok("stack.conf updated to match physical cores");
        } else {
            out_.warn("could not rewrite stack.conf — edit it by hand");
        }
    } else {
        out_.ok("stack.conf already on physical-core budget");
    }

    out_.section("llama.cpp (adaptive KV streaming)");
    std::error_code ec;
    // A missing tool is named before anything runs, never discovered as a bare "build failed".
    const bool built = ::access(c.server().c_str(), X_OK) == 0;
    std::vector<std::string> missing;
    if (!fs::is_directory(c.llama_dir() / ".git", ec) && which("git").empty()) missing.push_back("git");
    if (!built && which("cmake").empty()) missing.push_back("cmake");
    if (!built && which("c++").empty() && which("g++").empty() && which("clang++").empty()) missing.push_back("a C++ compiler (g++)");
    if (!missing.empty()) {
        std::string list;
        for (const auto& m : missing) list += (list.empty() ? "" : ", ") + m;
        out_.bad("cannot build llama.cpp", "— missing: " + list + "; install it and rerun: ariadne limbo setup");
        return 1;
    }
    if (!fs::is_directory(c.llama_dir() / ".git", ec)) {
        fs::create_directories(c.llama_dir().parent_path(), ec);
        if (run({"git", "clone", "--branch", c.str("LLAMA_BRANCH", "feature/adaptive-kv-stream"),
                 c.str("LLAMA_REPO"), c.llama_dir().string()}) != 0) {
            out_.bad("clone failed");
            return 1;
        }
    } else {
        out_.ok("already cloned");
    }
    if (::access(c.server().c_str(), X_OK) != 0) {
        const bool cuda = !which("nvcc").empty();
        if (!cuda) out_.warn("no nvcc", "— building for the CPU; install the CUDA toolkit and rerun setup for the GPU");
        out_.line("  building (this takes a while)…");
        std::vector<std::string> cfg = {"cmake", "-S", c.llama_dir().string(), "-B", (c.llama_dir() / "build").string(),
                                        "-DCMAKE_BUILD_TYPE=Release"};
        if (cuda) cfg.insert(cfg.end(), {"-DGGML_CUDA=ON", "-DGGML_CUDA_FA_ALL_QUANTS=ON"});
        // A bare -j is unbounded: on a 4-core, 6 GiB box beside a running pod the OOM killer took
        // the compilers and the machine stopped answering. Jobs = cores, or what free RAM holds at
        // ~1.5 GiB per compiler, whichever is smaller.
        unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
        {
            std::ifstream mi("/proc/meminfo");
            std::string line;
            while (std::getline(mi, line)) {
                if (line.rfind("MemAvailable:", 0) != 0) continue;
                const double gib = std::atof(line.c_str() + 13) / (1024.0 * 1024.0);
                jobs = std::max(1u, std::min(jobs, static_cast<unsigned>(gib / 1.5)));
                break;
            }
        }
        out_.dim("compiling with " + std::to_string(jobs) + " job(s) (cores and free RAM)");
        if (run(cfg) != 0 || run({"cmake", "--build", (c.llama_dir() / "build").string(), "--config", "Release",
                                  "--target", "llama-server", "-j", std::to_string(jobs)}) != 0) {
            out_.bad("build failed");
            return 1;
        }
    }
    out_.ok("llama-server ready");

    // limbo-engine: built where its tree is, with nvcc's temporaries inside it (never the shared /tmp)
    if (fs::is_directory(c.limbo_dir() / ".git", ec) && which("nvcc").size()) {
        const fs::path tmp = c.limbo_dir() / "build/tmp/ariadne-setup";
        fs::create_directories(tmp, ec);
        out_.section("limbo-engine");
        if (run({"env", "TMPDIR=" + tmp.string(), "nice", "-n", "10", "make", "-C", c.limbo_dir().string(), "-j",
                 std::to_string(std::max(1u, std::thread::hardware_concurrency() / 2)), "lib", "cuda", "serve-gpu"}) != 0) {
            out_.bad("limbo-engine build failed");
            if (default_profile(c) == "limbo") return 1;
        } else {
            out_.ok("limbo_serve_gpu ready");
        }
    } else if (default_profile(c) == "limbo") {
        out_.bad("limbo-engine not found at " + c.limbo_dir().string(), "— clone it there or set LIMBO_DIR / LIMBO_BIN");
        return 1;
    }

    out_.section("Models");
    if (which("hf").empty() && which("curl").empty()) { out_.bad("neither hf nor curl", "— cannot download models"); return 1; }
    if (!fetch(c.str("MAIN_REPO"), c.str("MAIN_FILE"), "main")) return 1;
    // the projector is a separate download; without it the first image 500s
    if (!c.str("MAIN_MMPROJ_FILE").empty() && !fetch(c.str("MAIN_REPO"), c.str("MAIN_MMPROJ_FILE"), "mmproj"))
        out_.warn("mmproj download failed", "— vision stays off");
    const std::string draft_repo = c.str("MAIN_DRAFT_REPO"), draft_file = c.str("MAIN_DRAFT_FILE");
    if (!draft_repo.empty() && !draft_file.empty() && draft_file[0] != '/' && draft_file[0] != '~'
        && draft_file.rfind("./", 0) != 0 && !fetch(draft_repo, draft_file, "DFlash2 draft"))
        out_.warn("DFlash2 draft download failed", "— falls back to MTP + ngram-mod");
    if (c.enabled("aux") && !fetch(c.str("AUX_REPO"), c.str("AUX_FILE"), "aux")) return 1;
    if (c.enabled("vis")) {
        if (!c.str("VIS_REPO").empty()) {
            fetch(c.str("VIS_REPO"), c.str("VIS_FILE"), "vision model");
            fetch(c.str("VIS_REPO"), c.str("VIS_MMPROJ_FILE"), "vision mmproj");
        } else {
            out_.warn("VIS_ENABLED=1 but VIS_REPO is empty", "— set it in " + paths_.conf().string());
        }
    }
    // fetched beforehand, so a nightly pass never finds it missing
    if (afe::enabled(c) && !fetch(afe::get(c, "AFE_REPO"), afe::get(c, "AFE_FILE"), "AFE (mazemaker)")) {
        out_.bad("AFE model download failed", "— retry: ariadne limbo afe fetch");
        return 1;
    }
    const auto [ar, af] = a3b_repo_file(c);
    if (!fetch(ar, af, "a3b (limbo start a3b)")) out_.warn("a3b model download failed", "— `ariadne limbo start a3b` stays unavailable");
    out_.line("");
    out_.ok("setup complete", "— ariadne limbo start");
    return 0;
}

int Stack::cmd_start(const std::string& requested) const {
    const StackConf c = conf();
    const std::string profile = requested.empty() ? default_profile(c) : requested;
    if (profile == "limbo" && ::access(c.limbo_server().c_str(), X_OK) != 0) {
        out_.bad("limbo_serve_gpu not built at " + c.limbo_server().string(), "— run: ariadne limbo setup");
        return 1;
    }
    if (profile != "strata" && profile != "limbo" && ::access(c.server().c_str(), X_OK) != 0) {
        out_.bad("llama-server not built", "— run: ariadne limbo setup");
        return 1;
    }
    const auto [model, how] = profile_model(c, profile);
    if (model.empty()) { out_.bad(profile + " model missing", "— run: " + how); return 1; }
    const std::string current = running_profile();
    if (state_of("main") != "stopped" && !current.empty() && current != profile) {
        out_.bad("main is already serving " + current + " — never two models", "— switch with: ariadne limbo restart " + profile);
        return 1;
    }
    if (afe::serving(*this, c)) {
        const auto [h, p] = afe::host_port(c);
        out_.bad("AFE model is serving on " + h + ":" + std::to_string(p) + " — never two models", "— stop it first: ariadne limbo afe stop");
        return 1;
    }
    out_.section("Starting (" + profile + ")");
    const auto [mh, mp] = c.host_port("main");
    if (state_of("main") == "stopped" && port_up(mh, mp)) {
        out_.bad(mh + ":" + std::to_string(mp) + " is already served by a process ariadne did not start",
                 "— never two models: stop that server, or keep using it as it is");
        return 1;
    }
    const Launch l = main_launch(c, profile);
    if (!start_one("main", l)) return 1;
    // main READY before the helper: two loads at once swap the machine.
    std::error_code ec;
    if (!wait_ready("main", mh, mp, profile == "strata" ? 900 : 300)) {
        fs::remove(paths_.run() / "main.profile", ec);
        return 1;
    }
    write_text(paths_.run() / "main.profile", profile);

    if (c.enabled("aux")) {
        const std::string aux = model_path(c.str("AUX_REPO"), c.str("AUX_FILE"), paths_);
        const auto [ah, ap] = c.host_port("aux");
        if (ap == mp) {
            out_.note("aux shares main's port (" + std::to_string(mp) + ") — served by slot 0 of the main process");
        } else if (!aux.empty()) {
            // --reasoning off: with thinking on a small helper spends its budget thinking
            Launch al;
            al.argv = {c.server().string(), "--model", aux, "--host", ah, "--port", std::to_string(ap),
                       "--ctx-size", std::to_string(c.integer("AUX_CTX", 12096)), "-ngl", "0",
                       "-t", std::to_string(c.integer("AUX_THREADS", 6)), "-np", "2", "-b", "512", "-ub", "512",
                       "--cont-batching", "--jinja", "--temp", "0.3", "--top-p", "0.9", "--reasoning", "off"};
            if (start_one("aux", al)) wait_ready("aux", ah, ap, 180);
        } else {
            out_.warn("aux model missing", "— skipping");
        }
    }
    if (c.enabled("vis")) {
        const std::string vis = model_path(c.str("VIS_REPO"), c.str("VIS_FILE"), paths_);
        const std::string vmm = model_path(c.str("VIS_REPO"), c.str("VIS_MMPROJ_FILE"), paths_);
        if (vis.empty() || vmm.empty()) {
            out_.warn("VIS_ENABLED=1 but model/projector missing", "— run: ariadne limbo setup");
        } else {
            const auto [vh, vp] = c.host_port("vis");
            Launch vl;
            vl.argv = {c.server().string(), "--model", vis, "-mm", vmm, "--host", vh, "--port", std::to_string(vp),
                       "--ctx-size", std::to_string(c.integer("VIS_CTX", 16384)), "-ngl", "0",
                       "-t", std::to_string(c.integer("VIS_THREADS", 4)), "-np", "2", "-b", "512", "-ub", "512",
                       "--cont-batching", "--jinja", "--reasoning", "off"};
            if (start_one("vis", vl)) wait_ready("vis", vh, vp, 300);
            out_.note("point auxiliary.vision.base_url at http://" + vh + ":" + std::to_string(vp) + "/v1");
        }
    }
    return 0;
}

int Stack::cmd_stop() const {
    out_.section("Stopping");
    // dependents first; Strata frees its expert arena on TERM, which takes longer than 10 s
    for (const char* n : {"vis", "aux", "main"})
        stop_one(n, std::string(n) == "main" && running_profile() == "strata" ? 120.0 : 10.0);
    std::error_code ec;
    fs::remove(paths_.run() / "main.profile", ec);
    return 0;
}

int Stack::cmd_restart(const std::string& requested) const {
    // no model named: restart what is running, never quietly a different profile
    std::string profile = requested;
    if (profile.empty()) profile = running_profile();
    if (profile.empty()) profile = default_profile(conf(false, false));
    cmd_stop();
    return cmd_start(profile);
}

namespace {
int signal_all(const Stack& s, int sig, const std::string& verb) {
    s.out().section(std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(verb[0])))) + verb.substr(1));
    bool touched = false;
    for (const char* n : kServers) {
        const pid_t pid = s.pid_of(n);
        if (!pid_alive(pid)) continue;
        if (::kill(pid, sig) == 0) { s.out().ok(std::string(n) + " " + verb); touched = true; }
        else s.out().bad(std::string(n) + " could not be " + verb, "(" + std::string(std::strerror(errno)) + ")");
    }
    if (!touched) s.out().warn("nothing running");
    return 0;
}
}  // namespace

int Stack::cmd_pause() const {
    const int r = signal_all(*this, SIGSTOP, "paused");
    out_.note("weights stay loaded; resume with: ariadne limbo resume");
    return r;
}

int Stack::cmd_resume() const { return signal_all(*this, SIGCONT, "resumed"); }

int Stack::cmd_logs(const std::string& name, int lines, bool follow) const {
    const std::string target = log_file(name.empty() ? "main" : name);
    std::error_code ec;
    if (!fs::exists(target, ec)) { out_.bad("no log for " + name, "(" + target + ")"); return 1; }
    std::vector<std::string> argv = {"tail", "-n", std::to_string(lines)};
    if (follow) argv.push_back("-f");
    argv.push_back(target);
    return run(argv);
}

int Stack::cmd_conf() const {
    std::error_code ec;
    const bool existed = fs::exists(paths_.conf(), ec);
    std::string note;
    const StackConf c = load_conf(paths_, true, &note);
    out_.section("Stack configuration");
    // a first run either imported the Python doctor's file or wrote the defaults: say which
    if (existed) out_.ok(paths_.conf().string());
    else out_.ok(paths_.conf().string(), "(" + (note.empty() ? std::string("created") : note) + ")");
    out_.note("llama.cpp   " + c.llama_dir().string());
    for (const char* n : kServers) {
        if (!c.enabled(n)) continue;
        const auto [h, p] = c.host_port(n);
        std::string up;
        for (const char* q = n; *q; ++q) up += static_cast<char>(std::toupper(static_cast<unsigned char>(*q)));
        std::string label = n;
        label.resize(11, ' ');
        out_.note(label + " " + c.str(up + "_REPO", "—") + " → " + h + ":" + std::to_string(p));
    }
    out_.note("logs        " + paths_.logs().string());
    out_.line("");
    return 0;
}

namespace {
std::string shell_quote(const std::string& s) {
    static const std::regex safe(R"(^[A-Za-z0-9@%+=:,./_-]+$)");
    if (!s.empty() && std::regex_match(s, safe)) return s;
    std::string q = "'";
    for (char ch : s) q += ch == '\'' ? std::string("'\"'\"'") : std::string(1, ch);
    return q + "'";
}
}  // namespace

std::string format_command(const Launch& l) {
    if (l.argv.empty()) return "";
    std::string first;
    for (const auto& [k, v] : l.env) first += k + "=" + shell_quote(v) + " ";
    first += shell_quote(l.argv[0]);
    std::vector<std::string> lines = {first};
    static const std::regex flag(R"(^--?[a-z])");
    for (size_t i = 1; i < l.argv.size();) {
        const bool takes = i + 1 < l.argv.size() && !std::regex_search(l.argv[i + 1], flag);
        std::string ln = "  " + shell_quote(l.argv[i]);
        if (takes) ln += " " + shell_quote(l.argv[i + 1]);
        lines.push_back(ln);
        i += takes ? 2 : 1;
    }
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) out += lines[i] + (i + 1 < lines.size() ? " \\\n" : "\n");
    return out;
}

int Stack::cmd_command(const std::string& requested) const {
    const StackConf c = conf(true, false);
    std::string profile = requested;
    if (profile.empty()) profile = running_profile();
    if (profile.empty()) profile = default_profile(c);
    Stack quiet = *this;
    quiet.out_.quiet = true;
    const Launch l = quiet.main_launch(c, profile);
    if (l.model.empty()) { out_.bad(profile + " model missing", "— run: " + profile_model(c, profile).second); return 1; }
    std::cout << format_command(l);
    return 0;
}

std::map<std::string, double> parse_metrics(const std::string& body) {
    std::map<std::string, double> out;
    std::istringstream in(body);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string name = line.substr(0, sp);
        if (name.rfind("llamacpp:", 0) != 0 || name.find('{') != std::string::npos) continue;
        char* end = nullptr;
        const std::string val = line.substr(sp + 1);
        const double v = std::strtod(val.c_str(), &end);
        if (end != val.c_str()) out[name.substr(9)] = v;
    }
    return out;
}

double sample_decode_rate(const std::map<std::string, double>& prev, const std::map<std::string, double>& cur) {
    if (prev.empty() || cur.empty()) return -1;
    auto at = [](const std::map<std::string, double>& m, const char* k) { const auto it = m.find(k); return it == m.end() ? 0.0 : it->second; };
    const double dt = at(cur, "tokens_predicted_total") - at(prev, "tokens_predicted_total");
    const double ds = at(cur, "tokens_predicted_seconds_total") - at(prev, "tokens_predicted_seconds_total");
    if (dt <= 0 || ds <= 0) return -1;   // idle must never look slow
    return dt / ds;
}

namespace {
std::map<std::string, double> scrape(const std::string& host, int port) {
    const net::Response r = net::get("http://" + probe_addr(host) + ":" + std::to_string(port) + "/metrics", 3000);
    return r.ok() ? parse_metrics(r.body) : std::map<std::string, double>{};
}
std::string fmt1(double v) { char b[32]; std::snprintf(b, sizeof b, "%.1f", v); return b; }
std::string fmtg(double v) { char b[32]; std::snprintf(b, sizeof b, "%g", v); return b; }
}  // namespace

int Stack::cmd_watch(double threshold, double interval, int trips_needed, double cooldown) const {
    const StackConf c = conf(true, false);
    const auto [host, port] = c.host_port("main");
    interval = std::max(5.0, interval);
    trips_needed = std::max(1, trips_needed);
    cooldown = std::max(0.0, cooldown);
    out_.section("Throughput watchdog");
    out_.dim(host + ":" + std::to_string(port) + " · restart below " + fmtg(threshold) + " tok/s for "
             + std::to_string(trips_needed) + " consecutive " + fmtg(interval) + "s windows · cooldown " + fmtg(cooldown) + "s");
    if (!port_up(host, port)) { out_.bad("main server is not up", "— nothing to watch"); return 1; }
    auto prev = scrape(host, port);
    if (prev.empty()) { out_.bad("/metrics returned nothing", "— the server needs --metrics (ariadne limbo start passes it)"); return 1; }
    std::signal(SIGINT, on_sigint);
    int trips = 0;
    double last_restart = 0;
    auto now = [] { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); };
    while (!g_interrupted.load()) {
        nap(interval);
        if (g_interrupted.load()) break;
        const auto cur = scrape(host, port);
        const double rate = sample_decode_rate(prev, cur);
        if (!cur.empty()) prev = cur;
        if (rate < 0) continue;
        if (rate >= threshold) {
            if (trips) out_.dim("recovered at " + fmt1(rate) + " tok/s");
            trips = 0;
            continue;
        }
        ++trips;
        out_.warn(fmt1(rate) + " tok/s", "— below " + fmtg(threshold) + " (" + std::to_string(trips) + "/" + std::to_string(trips_needed) + ")");
        if (trips < trips_needed) continue;
        const double since = now() - last_restart;
        if (last_restart > 0 && since < cooldown) { out_.dim("cooldown: " + std::to_string(static_cast<int>(cooldown - since)) + "s before another restart"); continue; }
        out_.bad("sustained slow generation", "— restarting the main server");
        std::string profile = running_profile();
        if (profile.empty()) profile = default_profile(c);
        stop_one("main");
        const Launch l = main_launch(c, profile);
        if (l.model.empty()) { out_.bad(profile + " model missing", "— cannot restart"); return 1; }
        if (!start_one("main", l)) return 1;
        write_text(paths_.run() / "main.profile", profile);
        if (!wait_ready("main", host, port, 300)) return 1;
        last_restart = now();
        trips = 0;
        prev = scrape(host, port);
    }
    out_.line("");
    out_.dim("watchdog stopped");
    return 0;
}

}  // namespace limbo
}  // namespace dd
