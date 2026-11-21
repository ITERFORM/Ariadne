// tests/doctor_test.cpp — `ariadne doctor`: stack.conf, model lookup, launch lines, processes,
// and mazemaker's AFE wake protocol. No GPU, no model: what is asserted is the doctor's own
// reasoning, and a `sleep` stands in for a server where a process is needed.
#include <arpa/inet.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <optional>

#include "dd/doctor/afe.hpp"
#include "dd/doctor/bench.hpp"
#include "dd/doctor/stack.hpp"
#include "dd/doctor/stack_conf.hpp"

namespace fs = std::filesystem;
using namespace dd::doctor;

namespace {
int g_fail = 0;
void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
    else std::printf("ok   %s\n", what.c_str());
}

bool has(const std::vector<std::string>& a, const std::string& x) {
    for (const auto& s : a) if (s == x) return true;
    return false;
}

std::string after(const std::vector<std::string>& a, const std::string& flag) {
    for (size_t i = 0; i + 1 < a.size(); ++i) if (a[i] == flag) return a[i + 1];
    return "";
}

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::trunc) << s;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
    const char* tmp_env = std::getenv("DD_TEST_TMP");
    const fs::path tmp = fs::absolute(fs::path(tmp_env ? tmp_env : "build/tmp") / "doctor");
    fs::remove_all(tmp);
    fs::create_directories(tmp);
    const fs::path fake_home = tmp / "user";
    fs::create_directories(fake_home);
    ::setenv("HOME", fake_home.c_str(), 1);
    ::setenv("HF_HUB_CACHE", (tmp / "hub").c_str(), 1);
    ::setenv("MAZEMAKER_DIR", (fake_home / ".mazemaker").c_str(), 1);
    ::setenv("DOCTOR_TEST_VAR", "xyz", 1);
    ::setenv("PATH", "/usr/bin:/bin", 1);   // no nvidia-smi, hf or systemd-run from a dev box

    // ---- parse_conf --------------------------------------------------------------------
    const Values v = parse_conf(
        "# comment\n"
        "A=plain\n"
        "B=\"quoted value\"  # comment after\n"
        "C='single'\n"
        "export D=exported\n"
        "E=7 # trailing\n"
        "F=\"$HOME/x\"\n"
        "G=${DOCTOR_TEST_VAR}/y\n"
        "H=$ARIADNE_HOME/llama\n"
        "I=$NOT_SET_ANYWHERE/z\n"
        "J=~/m\n"
        "not an assignment\n",
        {{"ARIADNE_HOME", "/opt/ar"}});
    chk(v.at("A") == "plain", "plain value");
    chk(v.at("B") == "quoted value", "double quotes, comment after the quote ignored");
    chk(v.at("C") == "single", "single quotes");
    chk(v.at("D") == "exported", "export prefix");
    chk(v.at("E") == "7", "inline comment dropped");
    chk(v.at("F") == fake_home.string() + "/x", "$HOME expanded");
    chk(v.at("G") == "xyz/y", "${VAR} from the environment");
    chk(v.at("H") == "/opt/ar/llama", "$ARIADNE_HOME from the doctor's vars");
    chk(v.at("I") == "$NOT_SET_ANYWHERE/z", "unknown $VAR kept as written");
    chk(v.at("J") == fake_home.string() + "/m", "~ expanded");
    chk(!v.count("not"), "a line that is no assignment is skipped");

    StackConf sc(Values{{"X", ""}, {"N", "12"}, {"BAD", "12x"}, {"F1", "1"}, {"AUX_PORT", "9000"}});
    chk(sc.str("X", "def") == "def", "empty counts as unset");
    chk(sc.integer("N", 0) == 12 && sc.integer("BAD", 5) == 5, "integer parses, garbage takes the default");
    chk(sc.flag("F1", false) && !sc.flag("missing", false), "flag");
    chk(sc.host_port("aux").second == 9000 && sc.host_port("vis").second == 8082 && sc.host_port("main").second == 8080,
        "ports: conf first, then 8080/8081/8082");
    chk(sc.enabled("aux") && !sc.enabled("vis") && sc.enabled("main"), "aux on, vis off by default");

    const StackConf def(parse_conf(kDefaultConf, {{"ARIADNE_HOME", "/opt/ar"}}));
    chk(def.str("MAIN_PROFILE") == "dense", "default profile is dense: it fits the smallest box");
    chk(def.llama_dir() == "/opt/ar/llama.cpp-adaptive-kv-streaming", "the fork builds inside the ariadne home");
    chk(def.integer("AFE_PORT", 0) == 8888 && def.integer("AFE_N_CPU_MOE", 0) == 18, "AFE knobs carried over");

    // ---- thread budget -----------------------------------------------------------------
    chk(propose_threads(16) == std::make_tuple(8, 4, 4), "16 cores -> 8/4/4");
    chk(propose_threads(12) == std::make_tuple(6, 3, 3), "12 cores -> 6/3/3");
    chk(propose_threads(8) == std::make_tuple(4, 2, 2), "8 cores -> 4/2/2");
    chk(propose_threads(4) == std::make_tuple(2, 1, 1), "4 cores -> 2/1/1");
    for (int n = 1; n <= 64; ++n) {
        const auto [m, a, vv] = propose_threads(n);
        if (n >= 3 && m + a + vv > n) { chk(false, "never above the physical count at " + std::to_string(n)); break; }
    }
    chk(physical_cores("physical id\t: 0\ncore id\t: 0\nphysical id\t: 0\ncore id\t: 0\n"
                       "physical id\t: 0\ncore id\t: 1\nphysical id\t: 1\ncore id\t: 0\n", 99) == 3,
        "hyperthread siblings counted once per (package, core)");
    chk(physical_cores("physical id\t: 0\ncpu cores\t: 6\n", 99) == 6, "falls back to cpu cores");
    chk(physical_cores("nothing useful\n", 5) == 5, "falls back to the caller's count");

    std::string conf_text = "# top\nMAIN_THREADS=8  # main\n  AUX_THREADS=6\nOTHER=1\n";
    chk(patch_threads(conf_text, 4, 2, 2), "patch reports a change");
    chk(conf_text.find("MAIN_THREADS=4  # main") != std::string::npos, "comment kept on the rewritten line");
    chk(conf_text.find("  AUX_THREADS=2") != std::string::npos, "indentation kept");
    chk(conf_text.find("VIS_THREADS=2") != std::string::npos && conf_text.find("OTHER=1") != std::string::npos,
        "missing key appended, the rest untouched");
    chk(!patch_threads(conf_text, 4, 2, 2), "nothing to change -> false");

    // ---- model lookup --------------------------------------------------------------------
    const Paths paths{tmp / "home"};
    const fs::path snap = tmp / "hub/models--org--name/snapshots/abc";
    write(tmp / "hub/models--org--name/blobs/deadbeef", "GGUF");
    fs::create_directories(snap);
    fs::create_symlink("../../blobs/deadbeef", snap / "m.gguf");
    chk(model_path("org/name", "m.gguf", paths) == (snap / "m.gguf").string(), "HF cache entry found through its symlink");
    chk(model_path("org/name", "other.gguf", paths).empty(), "a file the cache does not hold is ''");
    chk(model_path("", "m.gguf", paths).empty(), "no repo, no lookup");
    write(tmp / "plain.gguf", "x");
    chk(model_path("ignored", (tmp / "plain.gguf").string(), paths) == (tmp / "plain.gguf").string(), "a plain path is taken as is");
    write(local_model_file("org/curl", "c.gguf", paths), "x");
    chk(model_path("org/curl", "c.gguf", paths) == (paths.models() / "org--curl/c.gguf").string(),
        "a download without the hf CLI is found under models/");

    // ---- import from the Python doctor ---------------------------------------------------
    write(fake_home / ".daedalus/stack.conf", "MAIN_PROFILE=\"strata\"\nMAIN_THREADS=5\n");
    std::string note;
    const StackConf imported = load_conf(paths, true, &note);
    chk(imported.str("MAIN_PROFILE") == "strata" && imported.integer("MAIN_THREADS", 0) == 5, "measured values survive the move");
    chk(note.find("imported") != std::string::npos && fs::exists(paths.conf()), "stack.conf imported into the home, and said so");
    chk(fs::is_directory(paths.run()) && fs::is_directory(paths.logs()), "run/ and logs/stack/ created");
    fs::remove(paths.conf());
    fs::remove(fake_home / ".daedalus/stack.conf");
    const StackConf ro = load_conf(paths, false);
    chk(!fs::exists(paths.conf()) && ro.str("MAIN_PROFILE") == "dense", "read-only load leaves nothing behind");

    // ---- launch lines ----------------------------------------------------------------------
    Out quiet;
    quiet.quiet = true;
    Stack stack(paths, quiet);
    const StackConf lc(parse_conf(std::string(kDefaultConf) + "\nMAIN_DRAFT_REPO=\"\"\nMAIN_DRAFT_FILE=\"\"\n",
                                  {{"ARIADNE_HOME", paths.home.string()}}));
    const auto m = stack.main_argv(lc, "/models/main.gguf");
    chk(m[0] == paths.home.string() + "/llama.cpp-adaptive-kv-streaming/build/bin/llama-server", "the fork's llama-server");
    chk(after(m, "--kv-stream-stage-mib") == "1536" && has(m, "-kvu") && has(m, "--no-cache-idle-slots"),
        "KV pool with 3 slots -> -kvu and no idle-slot clearing");
    chk(after(m, "--spec-type") == "draft-mtp,ngram-mod", "no draft model -> MTP + ngram-mod");
    chk(!has(m, "--reasoning"), "reasoning auto sends no flag");
    chk(after(m, "--slot-save-path") == paths.slots().string(), "slot saves go to the ariadne home");
    chk(after(m, "-ctk") == "q8_0" && after(m, "-ctv") == "q4_0", "q8_0 K, q4_0 V");
    chk(has(m, "--metrics"), "metrics on for watch");
    const auto a3 = stack.a3b_argv(lc, "/models/a3b.gguf");
    chk(after(a3, "--n-cpu-moe") == "18" && after(a3, "-ub") == "2048" && after(a3, "-a") == "cybertiel-coder-35b-a3b",
        "a3b: experts in RAM, big micro-batch, its alias");
    const auto st = stack.strata_argv(lc, "/s/strata.json");
    chk(st[1].find("serve/server.py") != std::string::npos && after(st, "--config") == "/s/strata.json", "strata goes through its server");

    Launch fl;
    fl.env = {{"GGML_CUDA_ENABLE_UNIFIED_MEMORY", "1"}};
    fl.argv = {"/bin/llama-server", "--model", "/m/a b.gguf", "-fa", "on", "--metrics", "-kvu", "--temp", "-1"};
    const std::string cmd = format_command(fl);
    chk(cmd.rfind("GGML_CUDA_ENABLE_UNIFIED_MEMORY=1 /bin/llama-server \\\n", 0) == 0, "env first, then the binary");
    chk(cmd.find("  --model '/m/a b.gguf' \\\n") != std::string::npos, "a value with a space is quoted");
    chk(cmd.find("  --metrics \\\n  -kvu \\\n") != std::string::npos, "flags without values stand alone");

    // ---- metrics -------------------------------------------------------------------------------
    const auto m1 = parse_metrics("# HELP x\nllamacpp:tokens_predicted_total 100\nllamacpp:tokens_predicted_seconds_total 4\n"
                                  "llamacpp:labelled{a=\"b\"} 1\nother_metric 3\n");
    chk(m1.size() == 2 && m1.at("tokens_predicted_total") == 100, "llamacpp counters without labels");
    const auto m2 = parse_metrics("llamacpp:tokens_predicted_total 300\nllamacpp:tokens_predicted_seconds_total 8\n");
    chk(sample_decode_rate(m1, m2) == 50.0, "windowed rate from the counters");
    chk(sample_decode_rate(m1, m1) < 0, "an idle window is no verdict");
    chk(sample_decode_rate(m2, m1) < 0, "a counter going backwards (restart) is no verdict");
    chk(sample_decode_rate({}, m2) < 0, "a failed scrape is no verdict");

    // ---- AFE ---------------------------------------------------------------------------------
    const StackConf ac(parse_conf("AFE_SLOTS=2\n", {}));
    const auto aa = afe::argv(ac, "/m/afe.gguf", "secret");
    chk(after(aa, "--port") == "8888" && after(aa, "--host") == "0.0.0.0", "AFE on 0.0.0.0:8888 for the pod");
    chk(after(aa, "--api-key") == "secret" && has(aa, "-kvu") && after(aa, "--n-cpu-moe") == "18", "key, unified KV for 2 slots, experts in RAM");
    chk(afe::probe_host(ac) == "127.0.0.1", "0.0.0.0 is probed on loopback");
    write(fake_home / ".mazemaker/llm.toml", "[other]\napi_key = \"no\"\n[llm]\nmodel = \"x\"\napi_key = \"k123\"\n");
    chk(afe::api_key() == "k123", "the key from [llm], not another section");
    afe::share_key_with_pod();
    chk(slurp(afe::sockets_dir() / "llm.toml") == slurp(fake_home / ".mazemaker/llm.toml"), "llm.toml copied into sockets/ for the pod");

    // wake on a machine that cannot serve: the request is consumed and the reason left for the pod
    write(afe::request_file(), "1");
    const int rc = afe::cmd_wake(stack);
    chk(rc == 1, "wake refuses without a built server");
    chk(!fs::exists(afe::request_file()), "the request is consumed (the path unit must not loop)");
    const std::string failed = slurp(afe::failed_file());
    chk(failed.find("llama-server not built") != std::string::npos && failed.find("AFE model not on disk") != std::string::npos,
        "llm-on-demand.failed names every reason");
    chk(fs::exists(afe::last_used_file()), "last-used stamped on wake");
    chk(afe::cmd_idle_check(stack) == 0, "idle-check with nothing running is a no-op");

    // ---- probing a server bound to 0.0.0.0 -----------------------------------------------------
    chk(probe_addr("0.0.0.0") == "127.0.0.1" && probe_addr("::") == "127.0.0.1" && probe_addr("") == "127.0.0.1",
        "bind addresses are probed on loopback");
    chk(probe_addr("192.168.0.5") == "192.168.0.5" && probe_addr("127.0.0.1") == "127.0.0.1", "a real address is kept");
    {
        // A desktop conf binds main on 0.0.0.0; the net client refuses that address, so every
        // probe failed and start waited out its timeout on a model that was up.
        const int lfd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
        sa.sin_port = 0;
        const int one = 1;
        ::setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        const bool bound = ::bind(lfd, reinterpret_cast<sockaddr*>(&sa), sizeof sa) == 0 && ::listen(lfd, 4) == 0;
        socklen_t len = sizeof sa;
        ::getsockname(lfd, reinterpret_cast<sockaddr*>(&sa), &len);
        const int port = ntohs(sa.sin_port);
        std::thread srv([lfd] {
            for (int i = 0; i < 2; ++i) {
                const int c = ::accept(lfd, nullptr, nullptr);
                if (c < 0) return;
                char buf[1024];
                (void)::read(c, buf, sizeof buf);
                const char* resp = "HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\nok";
                (void)::write(c, resp, std::strlen(resp));
                ::close(c);
            }
        });
        chk(bound && health_ok("0.0.0.0", port, 3000), "health of a server bound to 0.0.0.0 is seen");
        chk(port_up("0.0.0.0", port, 3000), "port_up of a server bound to 0.0.0.0 is seen");
        srv.join();
        ::close(lfd);
    }
    chk(a3b_threads(StackConf(Values{{"MAIN_THREADS", "1"}}), 16) == 8, "a3b ignores MAIN_THREADS=1: 16 cores -> 8");
    chk(a3b_threads(StackConf(Values{}), 4) == 4 && a3b_threads(StackConf(Values{}), 64) == 12, "a3b threads within 4..12");
    chk(a3b_threads(StackConf(Values{{"A3B_THREADS", "6"}}), 16) == 6, "A3B_THREADS wins when set");

    // ---- bench --------------------------------------------------------------------------------
    using namespace dd::doctor::bench;
    const std::string p1 = long_prompt(500, 7), p2 = long_prompt(500, 984);
    chk(p1 == long_prompt(500, 7), "a prompt is reproducible from its salt");
    chk(p1 != p2, "every run's salt makes a new prompt (no cache help)");
    {
        const auto list = p1.substr(p1.find('\n') + 1, p1.find("\n\n") - p1.find('\n') - 1);
        size_t words = 1;
        for (char ch : list) words += ch == ' ' ? 1 : 0;
        chk(words == 500 && p1.rfind("Run 7: ", 0) == 0, "the prompt holds the asked number of words");
    }
    {
        const std::string fc = file_chunk(2, 1200);
        size_t lines = 1;
        for (char ch : fc) lines += ch == '\n' ? 1 : 0;
        chk(lines == 100 && fc == file_chunk(2, 1200) && fc != file_chunk(3, 1200), "file chunk: 1200 words -> 100 lines, per turn");
    }

    ChatResult cr;
    chk(sse_event(R"({"choices":[{"delta":{"role":"assistant"}}]})", 0.1, cr) && cr.first < 0, "an empty delta is no token");
    sse_event(R"({"choices":[{"delta":{"reasoning_content":"hm"}}]})", 0.5, cr);
    chk(cr.first == 0.5 && cr.pieces == 1 && cr.text.empty(), "reasoning counts as the first token, not as text");
    sse_event(R"({"choices":[{"delta":{"content":"Hi"}}]})", 0.6, cr);
    sse_event(R"({"choices":[{"delta":{"tool_calls":[{"function":{"name":"read_file"}}]}}]})", 0.7, cr);
    chk(cr.text == "Hi" && cr.pieces == 3, "content is the text; a tool call is produced output too");
    sse_event(R"({"choices":[],"timings":{"prompt_n":90,"cache_n":10,"prompt_per_second":500.5,"predicted_per_second":42.0,"draft_n":20,"draft_n_accepted":15}})", 0.8, cr);
    chk(!sse_event("[DONE]", 0.9, cr), "[DONE] ends the stream");
    chk(sse_event("not json", 1.0, cr), "a broken line is skipped, not fatal");
    cr.total = 2.0;
    const Digest dg = digest(cr);
    chk(dg.ok && dg.prompt_tokens && *dg.prompt_tokens == 100 && *dg.read_tokens == 90 && *dg.reused_tokens == 10,
        "usage null on a stream: prompt = prompt_n + cache_n");
    chk(dg.decode_tps && *dg.decode_tps == 42.0 && dg.decode_source == "engine", "decode speed is the engine's own");
    chk(*dg.drafts_offered == 20 && *dg.drafts_accepted == 15, "draft counters carried");
    ChatResult cl;
    cl.first = 1.0;
    cl.total = 3.0;
    cl.pieces = 41;
    const Digest dc = digest(cl);
    chk(dc.decode_tps && *dc.decode_tps == 20.0 && dc.decode_source == "client", "no timings: (gen-1)/(total-first) from the client");
    ChatResult ce;
    sse_event(R"({"error":{"message":"boom"}})", 0.1, ce);
    chk(!digest(ce).ok && digest(ce).err.find("boom") != std::string::npos, "a streamed error makes the request not ok");

    const auto s3 = stat({3.0, std::nullopt, 1.0, 2.0});
    chk(s3 && s3->median == 2.0 && s3->min == 1.0 && s3->max == 3.0 && s3->n == 3, "median of three, missing values ignored");
    chk(stat({1.0, 2.0, 3.0, 4.0})->median == 2.5, "an even count takes the mean of the middle two");
    chk(!stat({std::nullopt}), "nothing measured -> no stat");
    chk(*pctl({1.0, 2.0, 3.0, 4.0, 5.0}, 50) == 3.0 && *pctl({1.0, 2.0, 3.0, 4.0, 5.0}, 95) == 5.0, "percentiles by rank");
    chk(redact("/home/bob/x by bob on box1", "/home/bob", "bob", "box1") == "~/x by <user> on <host>", "home, user and host redacted");
    chk(slug("NVIDIA GeForce RTX 4060 Ti", 24) == "nvidia-geforce-rtx-4060", "gpu slug, cut at the limit");

    // ---- processes ----------------------------------------------------------------------------
    Launch sl;
    sl.argv = {"sleep", "30"};
    chk(stack.start_one("main", sl), "start_one launches");
    const pid_t pid = stack.pid_of("main");
    chk(pid > 0 && pid_alive(pid) && stack.state_of("main") == "running", "pid file written, process running");
    chk(stack.start_one("main", sl) && stack.pid_of("main") == pid, "a running server is not started twice");
    ::kill(pid, SIGSTOP);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    chk(stack.state_of("main") == "paused", "SIGSTOP reads as paused");
    stack.stop_one("main", 5.0);
    chk(!pid_alive(pid) && stack.state_of("main") == "stopped" && !fs::exists(stack.pid_file("main")),
        "a paused server is stopped (CONT before TERM) and its pid file removed");

    Launch dl;
    dl.argv = {"false"};
    chk(stack.start_one("aux", dl), "a server that will die still launches");
    chk(!stack.wait_ready("aux", "127.0.0.1", 1, 10), "wait_ready says it died instead of waiting out the timeout");

    Launch nl;
    nl.argv = {"definitely-not-a-binary-xyz"};
    chk(!stack.start_one("vis", nl), "a missing binary is refused before spawning");

    std::printf(g_fail ? "doctor: %d failed\n" : "doctor: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
