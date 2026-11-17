#pragma once
// dd/clip.hpp — text to the system clipboard: a helper (wl-copy / xclip / xsel) when one is installed,
// and always an OSC 52 write to the terminal, which is what reaches the clipboard over ssh and tmux.
//
// DD_CLIP=osc52 skips the helper, DD_CLIP=off copies nothing (headless boxes, tests).

#include <cstddef>
#include <functional>
#include <string>

namespace dd {
namespace clip {

constexpr size_t kMaxOsc52 = 100000;   // many terminals drop a longer OSC 52 payload

std::string base64(const std::string& in);

// "ESC ] 52 ; c ; <base64> BEL"; wrapped for passthrough when the terminal sits behind tmux.
std::string osc52(const std::string& payload, bool tmux = false);

struct Result {
    size_t bytes = 0;         // what was handed to the clipboard
    bool truncated = false;   // the OSC 52 copy was cut to kMaxOsc52 (a helper still got it whole)
};

// `write` carries the OSC 52 bytes to the terminal.
Result copy(const std::string& payload, const std::function<void(const std::string&)>& write);

}  // namespace clip
}  // namespace dd
