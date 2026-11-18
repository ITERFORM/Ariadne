// tests/clip_test.cpp — the clipboard both ways: base64, the OSC 52 frames (CLIPBOARD and PRIMARY, plain and behind
// tmux), the size cap, the env switches, and the helper path driven through a fake xclip on PATH, so the suite never
// touches the operator's clipboard. Scratch lives under $DD_TEST_TMP, never the shared /tmp.
#include "dd/clip.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace dd::clip;
namespace fs = std::filesystem;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
}

static std::string slurp(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

static void script(const fs::path& p, const std::string& body) {
    std::ofstream(p) << "#!/bin/sh\n" << body;
    fs::permissions(p, fs::perms::owner_all);
}

static bool wait_for(const fs::path& p, int ms) {
    for (int i = 0; i < ms / 10; ++i) {
        if (fs::exists(p)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return fs::exists(p);
}

int main() {
    // RFC 4648 section 10
    chk(base64("") == "", "base64: empty");
    chk(base64("f") == "Zg==", "base64: f");
    chk(base64("fo") == "Zm8=", "base64: fo");
    chk(base64("foo") == "Zm9v", "base64: foo");
    chk(base64("foob") == "Zm9vYg==", "base64: foob");
    chk(base64("fooba") == "Zm9vYmE=", "base64: fooba");
    chk(base64("foobar") == "Zm9vYmFy", "base64: foobar");
    chk(base64(std::string("\xff\xfe\x00", 3)) == "//4A", "base64: high bytes and NUL");

    chk(osc52("foo") == "\x1b]52;c;Zm9v\x07", "osc52: the CLIPBOARD frame");
    chk(osc52("foo", false, 'p') == "\x1b]52;p;Zm9v\x07", "osc52: the PRIMARY frame");
    chk(osc52("foo", true) == "\x1bPtmux;\x1b\x1b]52;c;Zm9v\x07\x1b\\", "osc52: behind tmux every ESC inside is doubled");

    const auto both = [](const std::string& t, bool tmux = false) { return osc52(t, tmux, 'c') + osc52(t, tmux, 'p'); };
    setenv("DD_CLIP", "osc52", 1);
    unsetenv("TMUX");
    {
        std::string wrote;
        const Result r = copy("hello", [&](const std::string& s) { wrote += s; });
        chk(wrote == both("hello") && r.bytes == 5 && !r.truncated,
            "copy: the terminal gets CLIPBOARD and PRIMARY (middle click and Shift+Insert paste PRIMARY)");
    }
    {
        std::string wrote;
        const std::string big(kMaxOsc52 + 500, 'a');
        const Result r = copy(big, [&](const std::string& s) { wrote += s; });
        chk(r.truncated && r.bytes == big.size(), "copy: over the cap the terminal copy is cut and says so");
        chk(wrote == both(std::string(kMaxOsc52, 'a')), "copy: each frame carries exactly the cap");
    }
    {   // a cut never lands inside a codepoint: 'é' is two bytes, straddling the cap
        std::string t(kMaxOsc52 - 1, 'a');
        t += "\xc3\xa9tail";
        std::string wrote;
        copy(t, [&](const std::string& s) { wrote += s; });
        chk(wrote == both(std::string(kMaxOsc52 - 1, 'a')), "copy: the cut backs up to a codepoint boundary");
    }
    setenv("TMUX", "/run/tmux-1000/default,1,0", 1);
    {
        std::string wrote;
        copy("foo", [&](const std::string& s) { wrote += s; });
        chk(wrote == both("foo", true), "copy: inside tmux both frames are wrapped for passthrough");
    }
    unsetenv("TMUX");
    {
        std::string wrote;
        const Result r = copy("", [&](const std::string& s) { wrote += s; });
        chk(wrote.empty() && r.bytes == 0, "copy: an empty text is no copy");
    }
    // with no helper the paste is the last copy, escapes and control bytes out
    copy("\x1b[31mred\x1b[0m line\r\nnext", [](const std::string&) {});
    chk(paste(false) == "red line\nnext", "paste: with no helper it is the last copy, sanitized");
    chk(paste(true) == "red line\nnext", "paste: middle click falls back the same way");
    setenv("DD_CLIP", "off", 1);
    {
        std::string wrote;
        const Result r = copy("secret", [&](const std::string& s) { wrote += s; });
        chk(wrote.empty() && r.bytes == 0, "copy: DD_CLIP=off writes nothing anywhere");
        chk(paste(false).empty(), "paste: DD_CLIP=off pastes nothing");
    }

    // ---- the helper path, through a fake xclip on PATH ----
    const char* e = std::getenv("DD_TEST_TMP");
    const fs::path root = fs::path(e && *e ? e : "build/tmp") / ("clip-" + std::to_string(::getpid()));
    fs::remove_all(root);
    const fs::path bin = root / "bin", out = root / "out", hung = root / "hung", none = root / "none", gone = root / "gone";
    for (const auto& d : {bin, out, hung, none, gone}) fs::create_directories(d);
    const std::string path0 = std::getenv("PATH") ? std::getenv("PATH") : "/usr/bin:/bin";

    // A descriptor the TUI holds open without CLOEXEC: no helper may inherit it.
    const int nul = ::open("/dev/null", O_RDONLY);
    const int mark = ::fcntl(nul, F_DUPFD, 77);
    ::close(nul);
    script(bin / "xclip",
           "out='" + out.string() + "'\n"
           "if [ \"$1\" = \"-o\" ]; then printf 'from-%s\\r\\nline2' \"$3\"; exit 0; fi\n"
           "sel=\"$2\"\n"
           "cat > \"$out/in.$sel.part\"\n"
           "if [ -e /proc/$$/fd/" + std::to_string(mark) + " ]; then echo leaked > \"$out/fd.$sel\"; else echo clean > \"$out/fd.$sel\"; fi\n"
           "set -- $(cat /proc/$$/stat)\n"
           "if [ \"$6\" = \"$$\" ]; then echo own > \"$out/sid.$sel\"; else echo shared > \"$out/sid.$sel\"; fi\n"
           "mv \"$out/in.$sel.part\" \"$out/in.$sel\"\n");
    for (const auto& d : {bin, hung, gone}) script(d / "xsel", "exit 1\n");   // never the real one
    script(hung / "xclip", "sleep 5\n");
    script(gone / "xclip", "touch \"" + gone.string() + "/ran.$2\"\nexit 0\n");   // leaves without reading its stdin

    unsetenv("DD_CLIP");
    unsetenv("WAYLAND_DISPLAY");
    setenv("DISPLAY", ":fake", 1);
    setenv("PATH", (bin.string() + ":" + path0).c_str(), 1);
    {
        const std::string text = "payload \xcf\x80\nsecond line";
        std::string wrote;
        copy(text, [&](const std::string& s) { wrote += s; });
        chk(wrote == both(text), "helper: OSC 52 still goes to the terminal");
        const bool c = wait_for(out / "in.clipboard", 3000), p = wait_for(out / "in.primary", 3000);
        chk(c && slurp(out / "in.clipboard") == text, "helper: xclip got the whole text for CLIPBOARD");
        chk(p && slurp(out / "in.primary") == text, "helper: xclip got the whole text for PRIMARY");
        chk(slurp(out / "fd.clipboard") == "clean\n" && slurp(out / "fd.primary") == "clean\n",
            "helper: the TUI's descriptors stay out of the clipboard owner");
        chk(slurp(out / "sid.clipboard") == "own\n" && slurp(out / "sid.primary") == "own\n",
            "helper: the owner runs in a session of its own (a tab's hangup does not take the clipboard with it)");
        chk(paste(true) == "from-primary\nline2", "paste: middle click reads PRIMARY, CRLF folded");
        chk(paste(false) == "from-clipboard\nline2", "paste: ctrl-v reads CLIPBOARD");
    }
    setenv("PATH", none.string().c_str(), 1);   // no helper installed at all
    chk(paste(false) == "payload \xcf\x80\nsecond line", "paste: no helper on PATH falls back to the last copy");
    setenv("PATH", (hung.string() + ":" + path0).c_str(), 1);
    {
        const auto t0 = std::chrono::steady_clock::now();
        const std::string t = paste(true);
        const long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
        chk(t == "payload \xcf\x80\nsecond line" && ms < 2000,
            "paste: a hung owner is killed and the prompt gets the last copy (" + std::to_string(ms) + " ms)");
    }
    setenv("PATH", (gone.string() + ":" + path0).c_str(), 1);
    {   // the helper leaves before reading 1 MiB: the write is an EPIPE on the helper thread, never a dead TUI
        copy(std::string(1 << 20, 'x'), [](const std::string&) {});
        const bool ran = wait_for(gone / "ran.clipboard", 3000) && wait_for(gone / "ran.primary", 3000);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        struct sigaction sa {};
        ::sigaction(SIGPIPE, nullptr, &sa);
        chk(ran && sa.sa_handler == SIG_DFL, "helper: SIGPIPE is not ignored process-wide (children would inherit it)");
    }
    setenv("PATH", path0.c_str(), 1);
    fs::remove_all(root);

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("clip: all passed\n");
    return g_fail ? 1 : 0;
}
