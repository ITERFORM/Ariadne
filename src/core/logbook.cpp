// src/core/logbook.cpp — impl of dd/core/logbook.hpp. One descriptor, one ::write per record.
//
// The atomicity the header promises is POSIX's, not a hopeful assumption: an O_APPEND write() on a
// regular file places its data at the end of the file under the inode's write lock, so two threads'
// records land whole, one after the other. What that lock does NOT give is a bound on size, which
// is why a record is cut at kMaxRecord before it is written: a 40 KiB tool result must not become a
// 40 KiB line a tail has to reassemble.

#include "dd/core/logbook.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace dd {
namespace core {
namespace logbook {

namespace {

// A line above this is cut, not split. 8 KiB fits a tail, a journald line and a pipe.
constexpr size_t kMaxRecord = 8192;
// Rotation: one file of this size, then .1. Checked every kRotateEvery records per thread, so the
// fstat does not ride every line.
constexpr off_t kRotateBytes = 64 * 1024 * 1024;
constexpr int kRotateEvery = 256;

std::atomic<int> g_level{static_cast<int>(Level::Info)};
std::atomic<int> g_fd{-1};
std::atomic<bool> g_rotating{false};
std::atomic<std::uint64_t> g_written{0};
std::atomic<std::uint64_t> g_lost{0};
std::string g_path;                       // written by init() before any worker exists
std::vector<std::pair<std::string, std::string>> g_context;

long tid() noexcept {
#ifdef SYS_gettid
    return static_cast<long>(::syscall(SYS_gettid));
#else
    return 0;
#endif
}

// seconds, microseconds as the fraction — the precision a turn's stages need.
double now_s() noexcept {
    timespec ts{};
    ::clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

int open_at(const std::string& p) noexcept {
    if (p.empty()) return -1;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(p).parent_path(), ec);
    return ::open(p.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
}

void rotate_if_due(int fd) noexcept {
    if (g_path.empty() || g_rotating.exchange(true, std::memory_order_acq_rel)) return;
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_size > kRotateBytes) {
        const std::string old = g_path + ".1";
        ::rename(g_path.c_str(), old.c_str());   // the current fd keeps writing to the renamed inode
        const int fresh = open_at(g_path);
        if (fresh >= 0) {
            const int prev = g_fd.exchange(fresh, std::memory_order_acq_rel);
            if (prev >= 0) ::close(prev);
        }
    }
    g_rotating.store(false, std::memory_order_release);
}

int sink() noexcept {
    int fd = g_fd.load(std::memory_order_acquire);
    if (fd >= 0) return fd;
    if (g_path.empty()) return -1;
    const int opened = open_at(g_path);
    if (opened < 0) return -1;
    int expected = -1;
    if (g_fd.compare_exchange_strong(expected, opened, std::memory_order_acq_rel)) return opened;
    ::close(opened);   // another thread won the open; use its descriptor
    return expected;
}

}  // anon

Json A(const std::vector<std::string>& names) {
    Json arr = json_arr();
    for (const auto& n : names) arr_push(arr, json_str(n));
    return arr;
}

std::string head(const std::string& s, size_t n) {
    if (s.size() <= n) return s;
    return s.substr(0, n) + "…(+" + std::to_string(s.size() - n) + "B)";
}

std::string endpoint(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return std::string();
    size_t begin = scheme + 3;
    const size_t stop = url.find_first_of("/?#", begin);
    const size_t at = url.find('@', begin);
    if (at != std::string::npos && (stop == std::string::npos || at < stop)) begin = at + 1;
    const size_t end = url.find_first_of("/?#", begin);
    return url.substr(0, begin) + url.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

void init(const std::string& path, Level level) {
    g_path = path;
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

void close() noexcept {
    const int fd = g_fd.exchange(-1, std::memory_order_acq_rel);
    if (fd >= 0) ::close(fd);
}

void context(const std::string& key, const std::string& value) {
    if (key.empty()) return;
    for (auto& [k, v] : g_context)
        if (k == key) { v = value; return; }
    g_context.emplace_back(key, value);
}

void set_level(Level l) noexcept { g_level.store(static_cast<int>(l), std::memory_order_relaxed); }
Level level() noexcept { return static_cast<Level>(g_level.load(std::memory_order_relaxed)); }
bool on(Level l) noexcept {
    if (l == Level::Off) return false;
    return static_cast<int>(l) <= g_level.load(std::memory_order_relaxed);
}

const char* word(Level l) noexcept {
    switch (l) {
        case Level::Off: return "off";
        case Level::Error: return "error";
        case Level::Warn: return "warn";
        case Level::Info: return "info";
        case Level::Debug: return "debug";
        case Level::Full: return "full";
    }
    return "info";
}

Level from_word(const std::string& w, bool* ok) {
    std::string s;
    s.reserve(w.size());
    for (char c : w) s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (ok) *ok = true;
    if (s == "off" || s == "none" || s == "silent") return Level::Off;
    if (s == "error") return Level::Error;
    if (s == "warn" || s == "warning") return Level::Warn;
    if (s == "info" || s == "normal") return Level::Info;
    if (s == "debug") return Level::Debug;
    if (s == "full" || s == "trace" || s == "max" || s == "verbose") return Level::Full;
    if (ok) *ok = false;
    return level();
}

const std::string& path() noexcept { return g_path; }
std::uint64_t written() noexcept { return g_written.load(std::memory_order_relaxed); }
std::uint64_t lost() noexcept { return g_lost.load(std::memory_order_relaxed); }

void write_record(Json& r, Level l, const char* cat, const char* ev) noexcept {
    const int fd = sink();
    if (fd < 0) { g_lost.fetch_add(1, std::memory_order_relaxed); return; }
    // The call site's fields are already in r; the envelope is stamped around them. Insertion order
    // puts the envelope first in the file, which is what a human tail wants to read.
    Json out = json_obj();
    obj_set(out, "t", json_num(now_s()));
    obj_set(out, "lvl", json_str(word(l)));
    obj_set(out, "cat", json_str(cat ? cat : "-"));
    obj_set(out, "ev", json_str(ev ? ev : "-"));
    obj_set(out, "pid", json_num(static_cast<double>(::getpid())));
    obj_set(out, "tid", json_num(static_cast<double>(tid())));
    for (const auto& [k, v] : g_context) obj_set(out, k, json_str(v));
    for (const auto& [k, v] : r.obj) obj_set(out, k, *v);

    std::string line;
    try {
        line = json_dump(out);
    } catch (...) {
        g_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (line.size() + 1 > kMaxRecord) {
        // Cut, do not split: keep the envelope, carry the overflow as a string field so the line
        // stays parseable JSON and the reader can see that something was cut. The head shrinks
        // until the dumped line fits: a body of quotes and backslashes grows under escaping, and a
        // line over the cap would be the one thing this writer promises never to write.
        const std::string keep = line;
        size_t want = kMaxRecord - 400;
        for (;;) {
            Json cut = json_obj();
            obj_set(cut, "t", json_num(now_s()));
            obj_set(cut, "lvl", json_str(word(l)));
            obj_set(cut, "cat", json_str(cat ? cat : "-"));
            obj_set(cut, "ev", json_str(ev ? ev : "-"));
            obj_set(cut, "pid", json_num(static_cast<double>(::getpid())));
            obj_set(cut, "tid", json_num(static_cast<double>(tid())));
            for (const auto& [k, v] : g_context) obj_set(cut, k, json_str(v));
            obj_set(cut, "cut", json_bool(true));
            obj_set(cut, "bytes", json_num(static_cast<double>(keep.size())));
            if (want > 0) obj_set(cut, "raw", json_str(head(keep, want)));
            try {
                line = json_dump(cut);
            } catch (...) {
                g_lost.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (line.size() + 1 <= kMaxRecord || want == 0) break;
            want = want > 512 ? want / 2 : 0;   // the worst case: escaping every byte
        }
        if (line.size() + 1 > kMaxRecord) {   // the envelope alone is over the cap: say so, write nothing
            g_lost.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    line.push_back('\n');

    ssize_t n = ::write(fd, line.data(), line.size());
    if (n != static_cast<ssize_t>(line.size())) {
        // A short write would leave a half line; count it and drop the tail rather than emit it.
        g_lost.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_written.fetch_add(1, std::memory_order_relaxed);

    static thread_local int since_check = 0;
    if (++since_check >= kRotateEvery) {
        since_check = 0;
        rotate_if_due(fd);
    }
}

void emit(Level l, const char* cat, const char* ev, std::initializer_list<Field> fields) {
    if (!on(l)) return;
    Json r = json_obj();
    for (const auto& f : fields)
        if (f.key) obj_set(r, f.key, f.value);
    write_record(r, l, cat, ev);
}

}  // namespace logbook
}  // namespace core
}  // namespace dd