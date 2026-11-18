#pragma once
// dd/clip.hpp — the system clipboard, both ways.
//
// A copy lands in CLIPBOARD and in PRIMARY: X11's middle click and Shift+Insert paste PRIMARY, so a copy that
// only filled CLIPBOARD was a copy nobody on i3 could paste. It goes through a helper (wl-copy / xclip / xsel)
// when one is installed, and always through OSC 52 to the terminal, which is what reaches the clipboard over
// ssh and tmux. A paste reads the same helpers and falls back to the last copy when there are none.
//
// DD_CLIP=osc52 skips the helpers, DD_CLIP=off copies and pastes nothing (headless boxes, tests).

#include <cstddef>
#include <functional>
#include <string>

namespace dd {
namespace clip {

constexpr size_t kMaxOsc52 = 100000;   // many terminals drop a longer OSC 52 payload
constexpr size_t kMaxPaste = 1 << 20;  // a paste past this is cut, not a frozen prompt

std::string base64(const std::string& in);

// "ESC ] 52 ; <sel> ; <base64> BEL", sel 'c' = CLIPBOARD, 'p' = PRIMARY; wrapped for passthrough behind tmux.
std::string osc52(const std::string& payload, bool tmux = false, char sel = 'c');

struct Result {
    size_t bytes = 0;         // what was handed to the clipboard
    bool truncated = false;   // the OSC 52 copy was cut to kMaxOsc52 (a helper still got it whole)
};

// `write` carries the OSC 52 bytes to the terminal. The helpers run on a thread of their own.
Result copy(const std::string& payload, const std::function<void(const std::string&)>& write);

// The text to paste: PRIMARY first when `primary` (then CLIPBOARD), else CLIPBOARD; escapes and control bytes
// out. Empty when there is nothing. A helper that hangs (a dead selection owner) is killed after ~0.6 s.
std::string paste(bool primary);

}  // namespace clip
}  // namespace dd
