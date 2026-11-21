#pragma once
// dd/limbo/stack.hpp — the machine's own llama.cpp servers, owned by `ariadne limbo`.
//
//   main  the model the harness talks to (profile dense, a3b or strata), MAIN_PORT
//   aux   a CPU helper on its own port, or slot 0 of main when it shares the port
//   vis   an optional CPU vision endpoint
//   afe   mazemaker's extraction model on :8888 (dd/limbo/afe.hpp), never beside the others
//
// A server is a pid file in <home>/run and a log in <home>/logs/stack. It runs in its own
// session, so it outlives the CLI that started it.

#include <map>
#include <string>
#include <utility>
#include <vector>
#include <sys/types.h>

#include "dd/limbo/stack_conf.hpp"

namespace dd {
namespace limbo {

// The glyphs of the report: ✓ ⚠ ✗ · and ◆ section headers, coloured on a terminal.
struct Out {
    bool color = false;
    bool quiet = false;   // builders called for `command` say nothing
    void ok(const std::string& text, const std::string& detail = "") const;
    void warn(const std::string& text, const std::string& detail = "") const;
    void bad(const std::string& text, const std::string& detail = "") const;
    void dim(const std::string& text) const;
    void note(const std::string& text) const;
    void section(const std::string& text) const;
    void line(const std::string& text) const;
};

// ---- processes limbo runs and waits for -----------------------------------------------
std::string which(const std::string& name);
// Inherits this terminal; returns the exit code (127 when it could not run).
int run(const std::vector<std::string>& argv, const std::string& cwd = "");
// stdout captured (stderr dropped); -1 on failure to run.
int run_capture(const std::vector<std::string>& argv, std::string& out, int timeout_s = 30);

// ---- servers ------------------------------------------------------------------------
struct Launch {
    std::string model;                                         // "" = not on disk
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    std::string cwd;
};

extern const char* const kServers[3];        // main aux vis
extern const char* const kProfiles[3];       // dense a3b strata

class Stack {
public:
    Stack(Paths paths, Out out) : paths_(std::move(paths)), out_(out) {}

    const Paths& paths() const { return paths_; }
    const Out& out() const { return out_; }
    Out& out() { return out_; }
    StackConf conf(bool create = true, bool announce = true) const;

    std::string pid_file(const std::string& name) const;
    std::string log_file(const std::string& name) const;
    pid_t pid_of(const std::string& name) const;
    std::string state_of(const std::string& name) const;   // running | paused | stopped

    bool start_one(const std::string& name, const Launch& l) const;
    bool wait_ready(const std::string& name, const std::string& host, int port, int seconds) const;
    void stop_one(const std::string& name, double grace_s = 10.0) const;

    std::string running_profile() const;
    std::string default_profile(const StackConf& c) const;
    std::string strata_config(const StackConf& c) const;
    // (path or "", the command that would fetch it)
    std::pair<std::string, std::string> profile_model(const StackConf& c, const std::string& profile) const;
    std::pair<std::string, std::string> a3b_repo_file(const StackConf& c) const;
    std::string mmproj(const StackConf& c) const;

    Launch main_launch(const StackConf& c, const std::string& profile) const;
    std::vector<std::string> main_argv(const StackConf& c, const std::string& model) const;
    std::vector<std::string> a3b_argv(const StackConf& c, const std::string& model) const;
    std::vector<std::string> strata_argv(const StackConf& c, const std::string& cfg) const;

    // `ariadne limbo <cmd>`. Each returns the process exit code.
    int cmd_status(const StackConf* c = nullptr) const;
    int cmd_report() const;
    int cmd_setup() const;
    int cmd_start(const std::string& profile) const;
    int cmd_stop() const;
    int cmd_restart(const std::string& profile) const;
    int cmd_pause() const;
    int cmd_resume() const;
    int cmd_logs(const std::string& name, int lines, bool follow) const;
    int cmd_conf() const;
    int cmd_command(const std::string& profile) const;
    int cmd_watch(double threshold, double interval, int trips, double cooldown) const;

    // A model on disk, or fetched: the hf CLI when there is one, else curl into paths().models().
    bool fetch(const std::string& repo, const std::string& file, const std::string& label) const;

private:
    Paths paths_;
    Out out_;
};

// Where to reach a server bound to `host`: 0.0.0.0, :: and "" are bind addresses, not
// destinations (and the net client refuses them), so they are probed on 127.0.0.1.
std::string probe_addr(const std::string& host);
bool pid_alive(pid_t pid);
std::string proc_state(pid_t pid);
// Any HTTP answer at all (a loading llama-server answers 503).
bool port_up(const std::string& host, int port, long timeout_ms = 2000);
// /health answers 200: loaded and able to take work.
bool health_ok(const std::string& host, int port, long timeout_ms = 2000);
// Physical cores of this machine (/proc/cpuinfo).
int machine_physical_cores();
// Threads for the a3b profile: A3B_THREADS, else half the physical cores within 4..12.
long a3b_threads(const StackConf& c, int physical);

// llama-server's /metrics, `llamacpp:` counters without labels.
std::map<std::string, double> parse_metrics(const std::string& body);
// Generation tok/s between two scrapes; < 0 = no verdict (idle, restarted, failed).
double sample_decode_rate(const std::map<std::string, double>& prev, const std::map<std::string, double>& cur);
// One option per line: the flag plus its value when it takes one, shell-quoted.
std::string format_command(const Launch& l);

}  // namespace limbo
}  // namespace dd
