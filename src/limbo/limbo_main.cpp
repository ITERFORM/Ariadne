// src/limbo/limbo_main.cpp — `ariadne limbo <command>`.
#include "dd/limbo/limbo.hpp"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include "dd/limbo/afe.hpp"
#include "dd/limbo/bench.hpp"
#include "dd/limbo/stack.hpp"

namespace dd {
namespace limbo {

void limbo_usage(bool to_stderr) {
    std::fputs(
        "usage: ariadne limbo [command]\n"
        "\n"
        "  (none)            the whole report: toolchain, sources, models, servers, AFE\n"
        "  status            the servers: ports, pids, VRAM\n"
        "  stack             the same report (the Python doctor's name for it)\n"
        "  setup             clone and build llama.cpp, then fetch the models\n"
        "  start [profile]   start the servers: dense, a3b or strata (default MAIN_PROFILE)\n"
        "  stop              stop them, dependents first\n"
        "  restart [profile] stop, then start — the running profile unless one is named\n"
        "  pause | resume    freeze them with the weights loaded / unfreeze\n"
        "  conf              where stack.conf lives and the values in force\n"
        "  command [profile] the exact main-server launch line, ready to paste\n"
        "  watch [--threshold N] [--interval S] [--trips N] [--cooldown S]\n"
        "                    restart main when generation throughput collapses\n"
        "  logs [main|aux|vis|afe] [-n N] [--no-follow]\n"
        "  bench [profile] [--url URL] [--runs N] [--clients N] [--long] [--think] [--quick]\n"
        "        [--no-start] [--keep] [--out DIR] [--compare DIR]\n"
        "                    measure the model: decode, prefill, TTFT, cache reuse, VRAM\n"
        "  afe [status|fetch|start|stop|wake|idle-check]\n"
        "                    mazemaker's AFE model on :8888, never beside the stack\n"
        "\n"
        "stack.conf, run/ and logs/stack/ live in the ariadne home. A stack.conf left by the\n"
        "Python doctor in ~/.daedalus is imported on first use.\n",
        to_stderr ? stderr : stdout);
}

namespace {

bool valid_profile(const std::string& p) {
    for (const char* k : kProfiles) if (p == k) return true;
    return false;
}

int bad_args(const std::string& why) {
    std::fprintf(stderr, "ariadne limbo: %s\n", why.c_str());
    limbo_usage(true);
    return 2;
}

}  // namespace

int run_limbo(const std::vector<std::string>& args, const std::filesystem::path& home) {
    Out out;
    out.color = ::isatty(1) != 0 && !std::getenv("NO_COLOR");
    Stack s(Paths{home}, out);
    const std::string cmd = args.empty() ? "" : args[0];
    const std::string arg1 = args.size() > 1 ? args[1] : "";

    if (cmd == "-h" || cmd == "--help" || cmd == "help") { limbo_usage(false); return 0; }
    if (cmd.empty() || cmd == "stack") return s.cmd_report();
    if (cmd == "status") return s.cmd_status();
    if (cmd == "setup") return s.cmd_setup();
    if (cmd == "start" || cmd == "restart" || cmd == "command") {
        if (!arg1.empty() && !valid_profile(arg1)) return bad_args("unknown profile '" + arg1 + "' (dense, a3b, strata)");
        if (cmd == "start") return s.cmd_start(arg1);
        if (cmd == "restart") return s.cmd_restart(arg1);
        return s.cmd_command(arg1);
    }
    if (cmd == "stop") return s.cmd_stop();
    if (cmd == "pause") return s.cmd_pause();
    if (cmd == "resume") return s.cmd_resume();
    if (cmd == "conf") return s.cmd_conf();
    if (cmd == "watch") {
        double threshold = 20, interval = 30, cooldown = 300;
        int trips = 3;
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (i + 1 >= args.size()) return bad_args(a + " needs a value");
            const double v = std::atof(args[++i].c_str());
            if (a == "--threshold") threshold = v;
            else if (a == "--interval") interval = v;
            else if (a == "--trips") trips = static_cast<int>(v);
            else if (a == "--cooldown") cooldown = v;
            else return bad_args("unknown watch option " + a);
        }
        return s.cmd_watch(threshold, interval, trips, cooldown);
    }
    if (cmd == "logs") {
        std::string server = "main";
        int lines = 200;
        bool follow = true;
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "-n" || a == "--lines") {
                if (i + 1 >= args.size()) return bad_args(a + " needs a value");
                lines = std::atoi(args[++i].c_str());
            } else if (a == "--no-follow") {
                follow = false;
            } else if (a == "main" || a == "aux" || a == "vis" || a == "afe") {
                server = a;
            } else {
                return bad_args("unknown logs argument " + a);
            }
        }
        return s.cmd_logs(server, lines, follow);
    }
    if (cmd == "bench") {
        bench::Options o;
        for (size_t i = 1; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto val = [&]() -> const std::string* { return i + 1 < args.size() ? &args[++i] : nullptr; };
            if (a == "--long") o.long_ = true;
            else if (a == "--think") o.think = true;
            else if (a == "--quick") o.quick = true;
            else if (a == "--no-start") o.no_start = true;
            else if (a == "--keep") o.keep = true;
            else if (a == "--url" || a == "--runs" || a == "--clients" || a == "--out" || a == "--compare") {
                const std::string* v = val();
                if (!v) return bad_args(a + " needs a value");
                if (a == "--url") o.url = *v;
                else if (a == "--runs") o.runs = std::atoi(v->c_str());
                else if (a == "--clients") o.clients = std::atoi(v->c_str());
                else if (a == "--out") o.out = *v;
                else o.compare = *v;
            } else if (valid_profile(a)) {
                o.profile = a;
            } else {
                return bad_args("unknown bench argument " + a);
            }
        }
        return bench::cmd_bench(s, o);
    }
    if (cmd == "afe") {
        if (arg1.empty() || arg1 == "status") return afe::cmd_status(s);
        if (arg1 == "fetch") return afe::cmd_fetch(s);
        if (arg1 == "start") return afe::cmd_start(s);
        if (arg1 == "stop") return afe::cmd_stop(s);
        if (arg1 == "wake") return afe::cmd_wake(s);
        if (arg1 == "idle-check") return afe::cmd_idle_check(s);
        return bad_args("unknown afe action '" + arg1 + "'");
    }
    return bad_args("unknown command '" + cmd + "'");
}

}  // namespace limbo
}  // namespace dd
