#pragma once
// dd/core/logbook.hpp — the harness's one log: off by the default the TUI shows, full under
// /verbose full, and never a line on the alternate screen.
//
// WHY ONE FILE AND NO LOCK. Every thread may write: the turn, each delegate child, the router,
// the learner, the pod's poller. A queue with a mutex would serialize them, and the harness's
// hand-offs are lock-free by construction (slot rings, SnapCell, park). So a record is formatted
// whole on the caller's stack and handed to ONE ::write() on an O_APPEND descriptor: the kernel
// places the record under the inode's write lock, so records never interleave and no thread waits
// for another. A record over kMaxRecord is cut, never split.
//
// WHAT A RECORD IS. One JSON object per line:
//   {"t":1794180991.234567,"lvl":"info","cat":"tool","ev":"run","pid":…,"tid":…,"session":…,
//    "model":…,"name":"read_file","ms":12.4,"ok":true,"why":"<the model's intent>", …}
// `why` is a field, not a level: the router writes the signal and its reason, a tool writes the
// model's `intent`, the wire writes status and ms. That is the question the file answers — what
// did the harness do, when, and why — not a pile of printf debris.
//
// LEVELS. off < error < warn < info < debug < full.
//   info   starts, turns, failures, slow tools, retries, delegate spawn and join, pod failures
//   debug  every tool call, every wire request, every router signal
//   full   and the heads of the payloads: arguments, request body, response body (kHead bytes)
// The TUI never reads this file, and this file never writes to the TUI: a failed write is a
// counter (lost()), not a message over the alternate screen.
//
// THREAD OF CONTROL. init() once, on the main thread, after the home is resolved and before any
// worker starts; context() likewise. After that any thread may emit. close() at shutdown.

#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

#include "dd/core/json.hpp"

namespace dd {
namespace core {
namespace logbook {

enum class Level : int { Off = 0, Error = 1, Warn = 2, Info = 3, Debug = 4, Full = 5 };

// One field of a record. Build with the helpers below: S("read_file"), N(12.5), B(true), A({...}).
struct Field {
    const char* key;
    Json value;
};

inline Json S(std::string s) { return json_str(std::move(s)); }
inline Json N(double v) { return json_num(v); }
inline Json B(bool v) { return json_bool(v); }
// A list of names (offered uris, tool names) — the shape most records carry.
Json A(const std::vector<std::string>& names);

// A head of a payload for Full: the first n bytes, the rest counted, never silently dropped.
std::string head(const std::string& s, size_t n = 600);

// scheme://host[:port] of a url, with the path, the query and any userinfo cut away. A record may
// name the box a turn went to; it must not carry the token a provider puts in the url.
std::string endpoint(const std::string& url);

// ---- lifecycle ----
// path empty: no file, the log is off whatever the level says. Creates the parent directory.
void init(const std::string& path, Level level);
void close() noexcept;
// Fields on every record of this process: session, model, home. Call on the main thread after
// init and before the first turn (delegate children are threads and inherit it).
void context(const std::string& key, const std::string& value);

// ---- level ----
void set_level(Level l) noexcept;
Level level() noexcept;
bool on(Level l) noexcept;
const char* word(Level l) noexcept;                          // "off","error","warn","info","debug","full"
Level from_word(const std::string& w, bool* ok = nullptr);   // "full"/"trace"/"max" -> Full

// ---- write ----
// Emit when `l` is at or under the current level. The fields are built by the caller.
void emit(Level l, const char* cat, const char* ev, std::initializer_list<Field> fields = {});
// Emit when `l` is at or under the current level, building the record only then: for call sites
// whose fields cost something (a dumped body, a joined list). `build` fills the record's own
// object. Nothing is evaluated when the level is off.
template <class F>
void at(Level l, const char* cat, const char* ev, F&& build);

// ---- what the file did ----
const std::string& path() noexcept;   // "" until init
std::uint64_t written() noexcept;     // records that reached the file
std::uint64_t lost() noexcept;        // records dropped: no sink, a failed write, or cut short

// The one writer both emit() and at() use: stamps t/lvl/cat/ev/pid/tid/context onto r (already
// carrying the call site's fields) and writes the line. Exported for the template.
void write_record(Json& r, Level l, const char* cat, const char* ev) noexcept;

template <class F>
void at(Level l, const char* cat, const char* ev, F&& build) {
    if (!on(l)) return;
    Json r = json_obj();
    build(r);
    write_record(r, l, cat, ev);
}

}  // namespace logbook
}  // namespace core
}  // namespace dd