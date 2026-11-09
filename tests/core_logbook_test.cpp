// Verifies dd::core::logbook: the file, the levels, the cut, and the one property the whole design
// rests on — N threads writing at once produce N valid JSON lines, none interleaved.
#include "dd/core/logbook.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace dd::core;

static int failures = 0;
#define CHECK(cond, msg)                          \
    do {                                          \
        if (!(cond)) {                            \
            std::cerr << "FAIL: " << msg << "\n"; \
            ++failures;                           \
        }                                         \
    } while (0)

namespace {

std::vector<std::string> lines_of(const std::filesystem::path& p) {
    std::vector<std::string> out;
    std::ifstream in(p);
    std::string line;
    while (std::getline(in, line))
        if (!line.empty()) out.push_back(line);
    return out;
}

const Json* field(const Json& o, const char* k) { return json_get(o, k); }

}  // anon

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::path(std::getenv("DD_TEST_TMP") ? std::getenv("DD_TEST_TMP") : "/tmp") /
                         ("logbook_" + std::to_string(::getpid()));
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);

    // --- words and the level table ---
    {
        bool ok = true;
        CHECK(logbook::from_word("full", &ok) == logbook::Level::Full && ok, "full -> Full");
        CHECK(logbook::from_word("TRACE", &ok) == logbook::Level::Full && ok, "trace -> Full");
        CHECK(logbook::from_word("off", &ok) == logbook::Level::Off && ok, "off -> Off");
        CHECK(logbook::from_word("nonsense", &ok) == logbook::Level::Info && !ok, "unknown keeps level, says so");
        CHECK(std::string(logbook::word(logbook::Level::Debug)) == "debug", "word(debug)");
        CHECK(std::string(logbook::head("abcdef", 3)) == "abc…(+3B)", "head marks what it cut");
        CHECK(logbook::head("abc", 3) == "abc", "head keeps a short string whole");
    }

    // --- off: no file at all, not an empty one ---
    {
        const fs::path p = dir / "off.jsonl";
        logbook::init(p.string(), logbook::Level::Off);
        logbook::emit(logbook::Level::Error, "t", "e", {{"k", logbook::S("v")}});
        logbook::close();
        CHECK(!fs::exists(p), "off writes no file");
        CHECK(logbook::lost() == 0, "off is not a lost record: the caller asked for silence");
    }

    // --- info: error and info land, debug does not ---
    {
        const fs::path p = dir / "info.jsonl";
        logbook::init(p.string(), logbook::Level::Info);
        logbook::context("session", "s1");
        logbook::context("model", "m1");
        logbook::emit(logbook::Level::Error, "tool", "fail", {{"why", logbook::S("denied")}});
        logbook::emit(logbook::Level::Info, "agent", "turn", {{"n", logbook::N(3)}});
        logbook::emit(logbook::Level::Debug, "wire", "send", {{"ms", logbook::N(1)}});
        logbook::emit(logbook::Level::Full, "wire", "body", {{"raw", logbook::S("x")}});
        logbook::close();
        const auto lines = lines_of(p);
        CHECK(lines.size() == 2, "info keeps error+info, drops debug+full");
        if (lines.size() == 2) {
            bool parsed = true;
            for (const auto& l : lines) {
                try {
                    const Json o = json_parse(l);
                    const Json* lvl = field(o, "lvl");
                    parsed = parsed && lvl && lvl->type == Json::Str;
                    const Json* sess = field(o, "session");
                    parsed = parsed && sess && sess->type == Json::Str && sess->str == "s1";
                    const Json* t = field(o, "t");
                    parsed = parsed && t && t->type == Json::Num && t->num > 1.7e9;
                } catch (...) {
                    parsed = false;
                }
            }
            CHECK(parsed, "every line is JSON with the envelope and the context");
            try {
                const Json a = json_parse(lines[0]);
                const Json* why = field(a, "why");
                CHECK(why && why->type == Json::Str && why->str == "denied", "the call site's fields survive");
            } catch (...) {
                CHECK(false, "error record parses");
            }
        }
    }

    // --- full: the body head lands, and a record over the cap is cut, not split ---
    {
        const fs::path p = dir / "full.jsonl";
        logbook::init(p.string(), logbook::Level::Full);
        logbook::emit(logbook::Level::Full, "wire", "send", {{"raw", logbook::S(logbook::head(std::string(4000, 'a'), 600))}});
        logbook::emit(logbook::Level::Full, "tool", "big",
                      {{"blob", logbook::S(logbook::head(std::string(20000, 'b'), 20000))}});
        logbook::close();
        const auto lines = lines_of(p);
        CHECK(lines.size() == 2, "two records at full");
        if (lines.size() == 2) {
            CHECK(lines[0].size() < 8192, "first record inside the cap");
            CHECK(lines[1].size() <= 8192, "an oversized record is cut, never split");
            try {
                const Json b = json_parse(lines[1]);
                const Json* cut = field(b, "cut");
                CHECK(cut && cut->type == Json::Bool && cut->b, "the cut is declared");
                const Json* bytes = field(b, "bytes");
                CHECK(bytes && bytes->type == Json::Num && bytes->num > 8192, "the real size is kept");
                const Json* raw = field(b, "raw");
                CHECK(raw && raw->type == Json::Str && !raw->str.empty(), "a readable head rides along");
            } catch (...) {
                CHECK(false, "the cut record is still JSON");
            }
        }
    }

    // --- the lock-free claim, measured: 4 threads x 300 records, every line whole ---
    {
        const fs::path p = dir / "race.jsonl";
        logbook::init(p.string(), logbook::Level::Debug);
        constexpr int kThreads = 4, kEach = 300;
        std::vector<std::thread> ts;
        for (int th = 0; th < kThreads; ++th)
            ts.emplace_back([th] {
                for (int i = 0; i < kEach; ++i)
                    logbook::emit(logbook::Level::Debug, "race", "rec",
                                  {{"th", logbook::N(th)}, {"i", logbook::N(i)}, {"pad", logbook::S(std::string(64, 'p'))}});
            });
        for (auto& t : ts) t.join();
        logbook::close();
        const auto lines = lines_of(p);
        CHECK(static_cast<int>(lines.size()) == kThreads * kEach, "every record reached the file exactly once");
        int bad = 0, seen_th[kThreads] = {0, 0, 0, 0};
        for (const auto& l : lines) {
            try {
                const Json o = json_parse(l);
                const Json* th = field(o, "th");
                const Json* i = field(o, "i");
                const Json* pad = field(o, "pad");
                if (!th || !i || !pad || pad->str.size() != 64) {
                    ++bad;
                    continue;
                }
                const int t = static_cast<int>(th->num);
                if (t >= 0 && t < kThreads) ++seen_th[t];
            } catch (...) {
                ++bad;   // an interleaved pair of writes is exactly this: a line that will not parse
            }
        }
        CHECK(bad == 0, "no line interleaved under four writers");
        for (int t = 0; t < kThreads; ++t) CHECK(seen_th[t] == kEach, "each thread's records all present");
    }

    // --- a path that cannot be opened is a counter, never a throw or a stderr line ---
    {
        logbook::init((dir / "no_such_dir" / "x.jsonl").string(), logbook::Level::Info);
        fs::remove_all(dir / "no_such_dir", ec);
        const std::uint64_t before = logbook::lost();
        // A file where the directory should be: open() fails even for root.
        std::ofstream(dir / "no_such_dir") << "x";
        logbook::emit(logbook::Level::Error, "t", "e");
        logbook::close();
        CHECK(logbook::lost() == before + 1, "an unwritable sink counts the loss and stays quiet");
        fs::remove(dir / "no_such_dir", ec);
    }

    fs::remove_all(dir, ec);
    std::cout << (failures == 0 ? "logbook: all checks passed" : "logbook: FAILURES") << "\n";
    return failures == 0 ? 0 : 1;
}