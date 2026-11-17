#include "dd/clip.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

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

std::string osc52(const std::string& payload, bool tmux) {
    const std::string seq = "\x1b]52;c;" + base64(payload) + "\x07";
    if (!tmux) return seq;
    std::string esc;   // tmux passthrough: every ESC inside is doubled
    for (char c : seq) {
        if (c == '\x1b') esc += '\x1b';
        esc += c;
    }
    return "\x1bPtmux;" + esc + "\x1b\\";
}

namespace {

// One helper: payload on its stdin, output discarded. True when it ran and exited 0.
bool run_helper(const std::vector<std::string>& argv, const std::string& payload) {
    int fd[2];
    if (pipe(fd) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) {
        close(fd[0]);
        close(fd[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fd[0], STDIN_FILENO);
        const int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, STDOUT_FILENO);
            dup2(nul, STDERR_FILENO);
        }
        close(fd[0]);
        close(fd[1]);
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        execvp(a[0], a.data());
        _exit(127);
    }
    close(fd[0]);
    const char* p = payload.data();
    size_t n = payload.size();
    while (n) {
        const ssize_t w = write(fd[1], p, n);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) break;   // the helper died early: EPIPE, SIGPIPE is ignored for this call
        p += w;
        n -= static_cast<size_t>(w);
    }
    close(fd[1]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

// The first installed helper that takes the text wins. A thread, so a slow helper never stalls the frame.
void helper_async(std::string payload) {
    std::signal(SIGPIPE, SIG_IGN);   // a helper that exits before reading would otherwise end the TUI
    std::thread([payload = std::move(payload)] {
        std::vector<std::vector<std::string>> cands;
        const char* wl = std::getenv("WAYLAND_DISPLAY");
        const char* x = std::getenv("DISPLAY");
        if (wl && *wl) cands.push_back({"wl-copy"});
        if (x && *x) {
            cands.push_back({"xclip", "-selection", "clipboard"});
            cands.push_back({"xsel", "-ib"});
        }
        for (const auto& c : cands)
            if (run_helper(c, payload)) return;
    }).detach();
}

}  // namespace

Result copy(const std::string& payload, const std::function<void(const std::string&)>& write) {
    Result r;
    const char* mode = std::getenv("DD_CLIP");
    const std::string_view m = mode ? mode : "";
    if (m == "off" || payload.empty()) return r;
    if (m != "osc52") helper_async(payload);
    std::string osc = payload;
    if (osc.size() > kMaxOsc52) {
        size_t n = kMaxOsc52;
        while (n > 0 && (static_cast<unsigned char>(payload[n]) & 0xC0) == 0x80) --n;   // never cut a codepoint
        osc.resize(n);
        r.truncated = true;
    }
    const char* tm = std::getenv("TMUX");
    if (write) write(osc52(osc, tm && *tm));
    r.bytes = payload.size();
    return r;
}

}  // namespace clip
}  // namespace dd
