// tests/clip_test.cpp — the clipboard writer: base64, the OSC 52 frame (plain and behind tmux), the size cap, and the env switches.
// Only DD_CLIP=osc52 / off run here: no helper process is ever started by the suite.
#include "dd/clip.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

using namespace dd::clip;

static int g_fail = 0;
static void chk(bool ok, const std::string& what) {
    if (!ok) { ++g_fail; std::printf("FAIL %s\n", what.c_str()); }
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

    chk(osc52("foo") == "\x1b]52;c;Zm9v\x07", "osc52: the plain frame");
    chk(osc52("foo", true) == "\x1bPtmux;\x1b\x1b]52;c;Zm9v\x07\x1b\\", "osc52: behind tmux every ESC inside is doubled");

    setenv("DD_CLIP", "osc52", 1);
    unsetenv("TMUX");
    {
        std::string wrote;
        const Result r = copy("hello", [&](const std::string& s) { wrote += s; });
        chk(wrote == "\x1b]52;c;aGVsbG8=\x07" && r.bytes == 5 && !r.truncated, "copy: the terminal gets one OSC 52 frame");
    }
    {
        std::string wrote;
        const std::string big(kMaxOsc52 + 500, 'a');
        const Result r = copy(big, [&](const std::string& s) { wrote += s; });
        chk(r.truncated && r.bytes == big.size(), "copy: over the cap the terminal copy is cut and says so");
        chk(wrote.size() == osc52(std::string(kMaxOsc52, 'a')).size(), "copy: the frame carries exactly the cap");
    }
    {   // a cut never lands inside a codepoint: 'é' is two bytes, straddling the cap
        std::string t(kMaxOsc52 - 1, 'a');
        t += "\xc3\xa9";
        t += "tail";
        std::string wrote;
        copy(t, [&](const std::string& s) { wrote += s; });
        chk(wrote == osc52(std::string(kMaxOsc52 - 1, 'a')), "copy: the cut backs up to a codepoint boundary");
    }
    setenv("TMUX", "/tmp/tmux-1000/default,1,0", 1);
    {
        std::string wrote;
        copy("foo", [&](const std::string& s) { wrote += s; });
        chk(wrote == osc52("foo", true), "copy: inside tmux the frame is wrapped for passthrough");
    }
    unsetenv("TMUX");
    setenv("DD_CLIP", "off", 1);
    {
        std::string wrote;
        const Result r = copy("secret", [&](const std::string& s) { wrote += s; });
        chk(wrote.empty() && r.bytes == 0, "copy: DD_CLIP=off writes nothing anywhere");
    }
    setenv("DD_CLIP", "osc52", 1);
    {
        std::string wrote;
        const Result r = copy("", [&](const std::string& s) { wrote += s; });
        chk(wrote.empty() && r.bytes == 0, "copy: an empty text is no copy");
    }

    if (g_fail) std::printf("%d FAILED\n", g_fail);
    else std::printf("clip: all passed\n");
    return g_fail ? 1 : 0;
}
