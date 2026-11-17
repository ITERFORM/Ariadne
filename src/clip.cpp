#include "dd/clip.hpp"
#include "dd/tools/ansi_strip.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace dd {
namespace clip {

std::string base64(const std::string& in) {
    static constexpr char kTab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        const unsigned v = (static_cast<unsigned char>(in[i]) << 16) | (static_cast<unsigned char>(in[i + 1]) << 8) |
                           static_cast<unsigned char>(in[i + 2]);
        o += kTab[v >> 18];
        o += kTab[(v >> 12) & 63];
        o += kTab[(v >> 6) & 63];
        o += kTab[v & 63];
    }
    if (const size_t rest = in.size() - i; rest > 0) {
        unsigned v = static_cast<unsigned char>(in[i]) << 16;
        if (rest == 2) v |= static_cast<unsigned char>(in[i + 1]) << 8;
        o += kTab[v >> 18];
        o += kTab[(v >> 12) & 63];
        o += rest == 2 ? kTab[(v >> 6) & 63] : '=';
        o += '=';
    }
    return o;
}

std::string osc52(const std::string& payload, bool tmux, char sel) {
    const std::string seq = std::string("\x1b]52;") + sel + ";" + base64(payload) + "\x07";
    if (!tmux) return seq;
    std::string esc;   // tmux passthrough: every ESC inside is doubled
    for (char c : seq) {
        if (c == '\x1b') esc += '\x1b';
        esc += c;
    }
    return "\x1bPtmux;" + esc + "\x1b\\";
}

namespace {

using Argv = std::vector<std::string>;
using Clock = std::chrono::steady_clock;

std::string g_last;   // the last copy, the paste of last resort; key-loop thread only

std::string_view mode() {
    const char* m = std::getenv("DD_CLIP");
    return m ? m : "";
}

bool env_set(const char* k) {
    const char* v = std::getenv(k);
    return v && *v;
}

// A helper in a session of its own (a tab's hangup or a signal to the TUI's group never takes the clipboard owner
// with it), signals default, and none of the TUI's descriptors but the two it is handed (-1 = /dev/null).
// 0 or an errno, as posix_spawn.
int spawn(const Argv& argv, int in, int out, pid_t* pid) {
    std::vector<char*> a;
    for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
    a.push_back(nullptr);
#ifdef __GLIBC__
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (in >= 0) posix_spawn_file_actions_adddup2(&fa, in, 0);
    else posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out >= 0) posix_spawn_file_actions_adddup2(&fa, out, 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &all);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
    const int rc = ::posix_spawnp(pid, a[0], &fa, &at, a.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    return rc;
#else
    // Not glibc: no posix_spawn_file_actions_addclosefrom_np, so the child takes those steps by hand.
    const int nul = ::open("/dev/null", O_RDWR | O_CLOEXEC);
    const pid_t p = ::fork();
    if (p < 0) {
        const int e = errno;
        if (nul >= 0) ::close(nul);
        return e;
    }
    if (p == 0) {
        ::setsid();
        struct sigaction dfl {};
        dfl.sa_handler = SIG_DFL;
        for (int s = 1; s < NSIG; ++s) ::sigaction(s, &dfl, nullptr);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        ::dup2(in >= 0 ? in : nul, 0);
        ::dup2(out >= 0 ? out : nul, 1);
        ::dup2(nul, 2);
        for (int fd = 3; fd < 1024; ++fd) ::close(fd);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    if (nul >= 0) ::close(nul);
    *pid = p;
    return 0;
#endif
}

int exit_code(pid_t pid) {
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

std::vector<Argv> writers(bool primary) {
    std::vector<Argv> c;
    if (env_set("WAYLAND_DISPLAY")) c.push_back(primary ? Argv{"wl-copy", "--primary"} : Argv{"wl-copy"});
    if (env_set("DISPLAY")) {
        c.push_back({"xclip", "-selection", primary ? "primary" : "clipboard"});
        c.push_back({"xsel", primary ? "-ip" : "-ib"});
    }
    return c;
}

std::vector<Argv> readers(bool primary) {
    std::vector<Argv> c;
    if (env_set("WAYLAND_DISPLAY"))
        c.push_back(primary ? Argv{"wl-paste", "--no-newline", "--primary"} : Argv{"wl-paste", "--no-newline"});
    if (env_set("DISPLAY")) {
        c.push_back({"xclip", "-o", "-selection", primary ? "primary" : "clipboard"});
        c.push_back({"xsel", primary ? "-op" : "-ob"});
    }
    return c;
}

// One helper with `payload` on its stdin. True when it ran and exited 0.
bool put_with(const Argv& argv, const std::string& payload) {
    int fd[2];
    if (::pipe2(fd, O_CLOEXEC) != 0) return false;   // a shell call forked meanwhile must not hold the write end open
    pid_t pid = 0;
    const int rc = spawn(argv, fd[0], -1, &pid);
    ::close(fd[0]);
    if (rc != 0) {
        ::close(fd[1]);
        return false;
    }
    const char* p = payload.data();
    size_t n = payload.size();
    while (n) {
        const ssize_t w = ::write(fd[1], p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;   // the helper left before reading: EPIPE, the SIGPIPE is blocked on this thread
        p += w;
        n -= static_cast<size_t>(w);
    }
    ::close(fd[1]);
    return exit_code(pid) == 0;
}

// One helper's stdout, read until `deadline`. True when it exited 0 and said something.
bool get_with(const Argv& argv, Clock::time_point deadline, std::string& out) {
    int fd[2];
    if (::pipe2(fd, O_CLOEXEC) != 0) return false;
    pid_t pid = 0;
    const int rc = spawn(argv, -1, fd[1], &pid);
    ::close(fd[1]);
    if (rc != 0) {
        ::close(fd[0]);
        return false;
    }
    std::string got;
    bool hung = false;
    char buf[65536];
    for (;;) {
        const long left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        pollfd pf{fd[0], POLLIN, 0};
        const int r = left > 0 ? ::poll(&pf, 1, static_cast<int>(left)) : 0;
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) {
            hung = true;
            break;
        }
        const ssize_t n = ::read(fd[0], buf, sizeof buf);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (got.size() < kMaxPaste) got.append(buf, std::min(static_cast<size_t>(n), kMaxPaste - got.size()));
    }
    ::close(fd[0]);
    if (hung) ::kill(pid, SIGKILL);
    if (exit_code(pid) != 0 || hung || got.empty()) return false;
    out = std::move(got);
    return true;
}

// Both selections, the first helper that takes the text wins each. A thread, so a slow helper never stalls a frame.
void put_async(std::string payload) {
    std::thread([payload = std::move(payload)] {
        sigset_t pipe_sig;   // blocked on this thread only: a process-wide SIG_IGN would leak into every child
        sigemptyset(&pipe_sig);
        sigaddset(&pipe_sig, SIGPIPE);
        ::pthread_sigmask(SIG_BLOCK, &pipe_sig, nullptr);
        for (const bool primary : {false, true})
            for (const auto& c : writers(primary))
                if (put_with(c, payload)) break;
    }).detach();
}

}  // namespace

Result copy(const std::string& payload, const std::function<void(const std::string&)>& write) {
    Result r;
    const std::string_view m = mode();
    if (m == "off" || payload.empty()) return r;
    g_last = payload;
    if (m != "osc52") put_async(payload);
    std::string osc = payload;
    if (osc.size() > kMaxOsc52) {
        size_t n = kMaxOsc52;
        while (n > 0 && (static_cast<unsigned char>(payload[n]) & 0xC0) == 0x80) --n;   // never cut a codepoint
        osc.resize(n);
        r.truncated = true;
    }
    const bool tmux = env_set("TMUX");
    if (write) write(osc52(osc, tmux, 'c') + osc52(osc, tmux, 'p'));
    r.bytes = payload.size();
    return r;
}

std::string paste(bool primary) {
    const std::string_view m = mode();
    if (m == "off") return {};
    std::string t;
    if (m != "osc52") {
        const auto deadline = Clock::now() + std::chrono::milliseconds(600);
        const auto from = [&](bool sel) {
            for (const auto& c : readers(sel))
                if (get_with(c, deadline, t)) return true;
            return false;
        };
        if (!(primary && from(true))) from(false);
    }
    if (t.empty()) t = g_last;
    return dd::tools::sanitize_display_text(t);
}

}  // namespace clip
}  // namespace dd
