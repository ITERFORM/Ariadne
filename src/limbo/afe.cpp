// src/limbo/afe.cpp — mazemaker's AFE model on :8888. py: daedalus_cli/stack_afe.py.
#include "dd/limbo/afe.hpp"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

namespace dd {
namespace limbo {
namespace afe {

namespace {

const std::map<std::string, std::string>& defaults() {
    static const std::map<std::string, std::string> d = {
        {"AFE_ENABLED", "1"},
        {"AFE_REPO", "unsloth/Qwen3.6-35B-A3B-MTP-GGUF"},
        {"AFE_FILE", "Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf"},
        // the name mazemaker's engine release was measured against; a label, not a switch
        {"AFE_ALIAS", "qwen3.6-35b-a3b"},
        // the caller is in a podman pod and reaches the host as host.containers.internal;
        // the llm.toml key guards it
        {"AFE_HOST", "0.0.0.0"},
        {"AFE_PORT", "8888"},
        {"AFE_CTX", "16384"},
        {"AFE_SLOTS", "1"},
        {"AFE_NGL", "99"},
        // N=18: 8965 MiB, 60 tok/s, ~2 GiB free beside the AFE window worker (16 GiB card)
        {"AFE_N_CPU_MOE", "18"},
        // 0 = llama-server's own batch; a ubatch that holds the whole prompt keeps offloaded
        // experts' prefill on the GPU
        {"AFE_UBATCH", "0"},
        {"AFE_KV_UNIFIED", "1"},
        {"AFE_VRAM_BLOCK_MIB", "10240"},
        {"AFE_READY_TIMEOUT", "420"},
        {"AFE_IDLE_SECONDS", "600"},
    };
    return d;
}

std::string home() { const char* h = std::getenv("HOME"); return h ? h : ""; }

void write_file(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::trunc) << text;
}

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string epoch() { return std::to_string(static_cast<long long>(std::time(nullptr))); }

}  // namespace

std::string get(const StackConf& c, const std::string& key) {
    const auto it = defaults().find(key);
    return c.str(key, it == defaults().end() ? "" : it->second);
}

long num(const StackConf& c, const std::string& key) {
    const auto it = defaults().find(key);
    return c.integer(key, it == defaults().end() ? 0 : std::atol(it->second.c_str()));
}

bool enabled(const StackConf& c) { return c.flag("AFE_ENABLED", true); }
std::pair<std::string, int> host_port(const StackConf& c) { return {get(c, "AFE_HOST"), static_cast<int>(num(c, "AFE_PORT"))}; }

std::string probe_host(const StackConf& c) {
    const std::string h = get(c, "AFE_HOST");
    return (h == "0.0.0.0" || h == "::" || h.empty()) ? "127.0.0.1" : h;
}

std::string model(const StackConf& c, const Paths& p) { return model_path(get(c, "AFE_REPO"), get(c, "AFE_FILE"), p); }

fs::path sockets_dir() {
    const char* d = std::getenv("MAZEMAKER_DIR");
    return (d && *d ? fs::path(d) : fs::path(home()) / ".mazemaker") / "sockets";
}
fs::path request_file() { return sockets_dir() / "llm-on-demand.request"; }
fs::path failed_file() { return sockets_dir() / "llm-on-demand.failed"; }
fs::path last_used_file() { return sockets_dir() / "llm-last-used"; }

void report_failure(const std::string& reason) { write_file(failed_file(), epoch() + " " + reason + "\n"); }
void clear_failure() { std::error_code ec; fs::remove(failed_file(), ec); }

std::string api_key() {
    std::ifstream in(fs::path(home()) / ".mazemaker" / "llm.toml");
    std::string line;
    bool in_llm = false;
    static const std::regex key_re(R"re(^\s*api_key\s*=\s*["']([^"']*)["'])re");
    while (std::getline(in, line)) {
        const auto b = line.find_first_not_of(" \t");
        if (b != std::string::npos && line[b] == '[') { in_llm = line.compare(b, 5, "[llm]") == 0; continue; }
        std::smatch m;
        if (in_llm && std::regex_search(line, m, key_re)) return m[1].str();
    }
    return "";
}

void share_key_with_pod() {
    // The AFE window container mounts sockets/, never llm.toml itself: without this copy its
    // calls arrived without the key and got 401 on the first real pass.
    const fs::path src = fs::path(home()) / ".mazemaker" / "llm.toml";
    const fs::path dst = sockets_dir() / "llm.toml";
    std::error_code ec;
    if (!fs::exists(src, ec)) return;
    const std::string data = read_file(src);
    if (fs::exists(dst, ec) && read_file(dst) == data) return;
    fs::create_directories(dst.parent_path(), ec);
    const fs::path tmp = dst.string() + ".tmp";
    { std::ofstream(tmp, std::ios::binary | std::ios::trunc) << data; }
    ::chmod(tmp.c_str(), 0600);
    fs::rename(tmp, dst, ec);
}

long vram_used_mib() {
    if (which("nvidia-smi").empty()) return -1;
    std::string out;
    if (run_capture({"nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"}, out, 10) != 0) return -1;
    std::istringstream in(out);
    std::string first;
    std::getline(in, first);
    char* end = nullptr;
    const long v = std::strtol(first.c_str(), &end, 10);
    return (end != first.c_str()) ? v : -1;
}

std::vector<std::string> stack_serving(const Stack& s, const StackConf& c) {
    std::vector<std::string> serving;
    for (const char* n : kServers) {
        if (!c.enabled(n)) continue;
        const auto [h, p] = c.host_port(n);
        const std::string where = h + ":" + std::to_string(p);
        const pid_t pid = s.pid_of(n);
        if (pid_alive(pid)) serving.push_back(std::string(n) + " (pid " + std::to_string(pid) + ", " + where + ")");
        else if (port_up(h, p)) serving.push_back(std::string(n) + " (" + where + " answering, not started by ariadne)");
    }
    return serving;
}

bool serving(const Stack& s, const StackConf& c) {
    return pid_alive(s.pid_of(kName)) || port_up(probe_host(c), static_cast<int>(num(c, "AFE_PORT")));
}

std::vector<std::string> blockers(const Stack& s, const StackConf& c) {
    std::vector<std::string> reasons;
    if (!enabled(c)) reasons.push_back("AFE_ENABLED=0 in stack.conf");
    if (::access(c.server().c_str(), X_OK) != 0)
        reasons.push_back("llama-server not built (" + c.server().string() + ") — run: ariadne limbo setup");
    if (model(c, s.paths()).empty())
        reasons.push_back("AFE model not on disk: " + get(c, "AFE_REPO") + " / " + get(c, "AFE_FILE") + " — run: ariadne limbo afe fetch");
    const auto up = stack_serving(s, c);
    if (!up.empty()) {
        std::string list;
        for (const auto& u : up) list += (list.empty() ? "" : ", ") + u;
        reasons.push_back("never two models — the stack is serving: " + list + " — stop it first: ariadne limbo stop");
    }
    const long used = vram_used_mib();
    const long block = num(c, "AFE_VRAM_BLOCK_MIB");
    if (used >= 0 && block > 0 && used > block && !pid_alive(s.pid_of(kName)))
        reasons.push_back(std::to_string(used) + " MiB VRAM already in use (> " + std::to_string(block) + " MiB) — the card is taken");
    return reasons;
}

std::vector<std::string> argv(const StackConf& c, const std::string& gguf, const std::string& key) {
    const auto [h, p] = host_port(c);
    std::vector<std::string> a = {c.server().string(), "--model", gguf, "-a", get(c, "AFE_ALIAS"),
                                  "--host", h, "--port", std::to_string(p),
                                  "--ctx-size", std::to_string(num(c, "AFE_CTX")),
                                  "-ngl", std::to_string(num(c, "AFE_NGL")),
                                  "-np", std::to_string(num(c, "AFE_SLOTS")),
                                  "-fa", "on", "-ctk", "q8_0", "-ctv", "q8_0",
                                  "--jinja", "--reasoning-format", "deepseek", "--spec-type", "draft-mtp",
                                  "--temp", "0.6", "--top-p", "0.95", "--top-k", "20"};
    if (num(c, "AFE_N_CPU_MOE") > 0) a.insert(a.end(), {"--n-cpu-moe", std::to_string(num(c, "AFE_N_CPU_MOE"))});
    if (num(c, "AFE_UBATCH") > 0) a.insert(a.end(), {"-b", std::to_string(num(c, "AFE_UBATCH")), "-ub", std::to_string(num(c, "AFE_UBATCH"))});
    if (num(c, "AFE_SLOTS") > 1 && c.flag("AFE_KV_UNIFIED", true)) a.insert(a.end(), {"-kvu", "--no-cache-idle-slots"});
    if (!key.empty()) a.insert(a.end(), {"--api-key", key});
    return a;
}

bool healthy(const StackConf& c) { return health_ok(probe_host(c), static_cast<int>(num(c, "AFE_PORT"))); }

namespace {

// `wake` runs inside a oneshot unit; a plain child stays in its cgroup and is SIGKILLed with it
// when the oneshot ends, model and all. A transient unit of its own leaves that cgroup.
bool launch(const Stack& s, const std::vector<std::string>& cmd) {
    if (!which("systemd-run").empty() && !which("systemctl").empty()) {
        const std::string log = s.log_file(kName);
        std::error_code ec;
        fs::create_directories(fs::path(log).parent_path(), ec);
        std::string ignored;
        run_capture({"systemctl", "--user", "reset-failed", kUnit}, ignored, 10);
        std::vector<std::string> sr = {"systemd-run", "--user", "--unit", kUnit, "--collect", "--quiet",
                                       "-p", "StandardOutput=append:" + log, "-p", "StandardError=append:" + log, "--"};
        sr.insert(sr.end(), cmd.begin(), cmd.end());
        if (run_capture(sr, ignored, 30) == 0) {
            for (int i = 0; i < 50; ++i) {
                std::string pid;
                run_capture({"systemctl", "--user", "show", "-p", "MainPID", "--value", kUnit}, pid, 10);
                while (!pid.empty() && (pid.back() == '\n' || pid.back() == ' ')) pid.pop_back();
                if (!pid.empty() && pid != "0" && pid.find_first_not_of("0123456789") == std::string::npos) {
                    write_file(s.pid_file(kName), pid);
                    s.out().dim(std::string(kName) + " starting as " + kUnit + ".service (pid " + pid + ")…");
                    return true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            s.out().warn(std::string(kUnit) + ".service started but reported no MainPID");
            return false;
        }
        s.out().warn("systemd-run failed, starting as a plain child");
    }
    Launch l;
    l.argv = cmd;
    return s.start_one(kName, l);
}

bool wait_healthy(const Stack& s, const StackConf& c, long seconds) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (std::chrono::steady_clock::now() < deadline) {
        if (healthy(c)) return true;
        if (!pid_alive(s.pid_of(kName))) return false;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }
    return false;
}

}  // namespace

std::pair<bool, std::string> start(const Stack& s, const StackConf& c, int ready_timeout) {
    const long timeout = ready_timeout >= 0 ? ready_timeout : num(c, "AFE_READY_TIMEOUT");
    if (serving(s, c)) {
        if (healthy(c) || wait_healthy(s, c, timeout)) return {true, "already serving"};
        return {false, "AFE server is up but not healthy — ariadne limbo logs afe"};
    }
    const auto reasons = blockers(s, c);
    if (!reasons.empty()) {
        std::string joined;
        for (const auto& r : reasons) joined += (joined.empty() ? "" : "; ") + r;
        return {false, joined};
    }
    share_key_with_pod();
    if (!launch(s, argv(c, model(c, s.paths()), api_key()))) return {false, "llama-server failed to launch — ariadne limbo logs afe"};
    // used from the moment it starts: an old last-used stamp let the idle timer stop a loading model
    write_file(last_used_file(), epoch());
    if (!wait_healthy(s, c, timeout)) {
        const bool died = !pid_alive(s.pid_of(kName));
        s.stop_one(kName);
        return {false, std::string("AFE server ") + (died ? "died while loading" : "was not healthy within " + std::to_string(timeout) + "s")
                           + " (CUDA OOM? raise AFE_N_CPU_MOE) — ariadne limbo logs afe"};
    }
    return {true, "started"};
}

int cmd_status(const Stack& s, const StackConf* given) {
    const StackConf c = given ? *given : s.conf(false, false);
    const auto [h, p] = host_port(c);
    if (!enabled(c)) { s.out().dim("afe  disabled (AFE_ENABLED=0)"); return 0; }
    const std::string gguf = model(c, s.paths());
    if (!gguf.empty()) s.out().ok("afe model: " + fs::path(gguf).filename().string(), "(" + human_size(gguf) + ")");
    else s.out().bad("afe model missing", "— " + get(c, "AFE_REPO") + " — run: ariadne limbo afe fetch");
    const pid_t pid = s.pid_of(kName);
    const std::string where = h + ":" + std::to_string(p);
    if (pid_alive(pid)) {
        if (port_up(probe_host(c), p)) s.out().ok("afe  " + where + "  pid " + std::to_string(pid));
        else s.out().warn("afe  pid " + std::to_string(pid), "— process up, port not answering yet");
    } else if (port_up(probe_host(c), p)) {
        s.out().warn("afe  " + where + " answering", "— not started by ariadne (no pid file)");
    } else {
        const auto why = blockers(s, c);
        s.out().dim("afe  stopped" + (why.empty() ? std::string(" — ready to start") : " — would refuse to start: " + why.front()));
    }
    std::error_code ec;
    if (fs::exists(failed_file(), ec)) {
        std::string f = read_file(failed_file());
        while (!f.empty() && f.back() == '\n') f.pop_back();
        s.out().warn("last AFE wake failed", "— " + f);
    }
    return 0;
}

int cmd_fetch(const Stack& s) {
    const StackConf c = s.conf();
    s.out().section("AFE model");
    if (!model(c, s.paths()).empty()) { s.out().ok("afe model present", "— " + get(c, "AFE_FILE")); return 0; }
    if (!s.fetch(get(c, "AFE_REPO"), get(c, "AFE_FILE"), "AFE (mazemaker)")) {
        s.out().bad("AFE model download failed", "— " + get(c, "AFE_REPO") + " / " + get(c, "AFE_FILE"));
        return 1;
    }
    s.out().ok("afe model downloaded", "— " + get(c, "AFE_FILE"));
    return 0;
}

int cmd_start(const Stack& s) {
    const StackConf c = s.conf();
    s.out().section("AFE model");
    const auto [ok, reason] = start(s, c);
    if (ok) {
        const auto [h, p] = host_port(c);
        s.out().ok("afe serving " + get(c, "AFE_ALIAS") + " on " + h + ":" + std::to_string(p), "(" + reason + ")");
        return 0;
    }
    size_t pos = 0, next;
    while ((next = reason.find("; ", pos)) != std::string::npos) { s.out().bad("AFE not started", "— " + reason.substr(pos, next - pos)); pos = next + 2; }
    s.out().bad("AFE not started", "— " + reason.substr(pos));
    return 1;
}

int cmd_stop(const Stack& s) {
    s.out().section("AFE model");
    if (!pid_alive(s.pid_of(kName))) { s.out().dim("afe not running"); return 0; }
    s.stop_one(kName);
    return 0;
}

int cmd_wake(const Stack& s) {
    const StackConf c = s.conf(true, false);
    write_file(last_used_file(), epoch());
    // consume the request first: the path unit re-fires for as long as the file exists
    std::error_code ec;
    fs::remove(request_file(), ec);
    const auto [ok, reason] = start(s, c);
    if (ok) {
        clear_failure();
        write_file(last_used_file(), epoch());
        std::cout << "afe wake: " << reason << std::endl;
        return 0;
    }
    report_failure(reason);
    std::cout << "afe wake REFUSED: " << reason << std::endl;
    return 1;
}

int cmd_idle_check(const Stack& s) {
    const StackConf c = s.conf(false, false);
    if (!pid_alive(s.pid_of(kName))) return 0;
    std::string ignored;
    if (run_capture({"systemctl", "--user", "is-active", "--quiet", "mazemaker-afe-window.service"}, ignored, 10) == 0) return 0;
    const std::string t = read_file(last_used_file());
    char* end = nullptr;
    const double last = std::strtod(t.c_str(), &end);
    if (end == t.c_str()) return 0;
    const double idle = static_cast<double>(std::time(nullptr)) - last;
    if (idle >= static_cast<double>(num(c, "AFE_IDLE_SECONDS"))) {
        std::cout << "afe idle " << static_cast<long>(idle) << "s — stopping" << std::endl;
        s.stop_one(kName);
    }
    return 0;
}

}  // namespace afe
}  // namespace limbo
}  // namespace dd
