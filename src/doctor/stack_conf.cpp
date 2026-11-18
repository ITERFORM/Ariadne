// src/doctor/stack_conf.cpp — stack.conf parsing, defaults, model lookup, thread budget.
// py: daedalus_cli/stack.py (DEFAULT_CONF, _parse_conf, StackConf, model_path, gguf_str).
#include "dd/doctor/stack_conf.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace dd {
namespace doctor {

const char* const kDefaultConf = R"CONF(# Ariadne's local inference stack: `ariadne doctor`. Paths are absolute; ~ and
# $HOME / $ARIADNE_HOME are expanded on load. Nothing here is executed.

# --- the llama.cpp fork -------------------------------------------------------
# Adaptive KV streaming keeps the KV cache in pinned host memory, which is what
# lets a 27B model hold a six-figure context on a 16 GB card.
LLAMA_DIR="$ARIADNE_HOME/llama.cpp-adaptive-kv-streaming"
LLAMA_REPO="https://github.com/RaymondHuang210129/llama.cpp-adaptive-kv-streaming"
LLAMA_BRANCH="feature/adaptive-kv-stream"

# --- main model (profile dense) -------------------------------------------------
MAIN_REPO="ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF"
MAIN_FILE="Qwen3.8-27B-GSQ-RCO-IQ3_XXS-mtp.gguf"
MAIN_PORT=8080
MAIN_HOST="127.0.0.1"
# What a bare `ariadne doctor start` serves: dense (llama.cpp, fits the smallest
# box), a3b (the MoE below) or strata (needs a Strata checkout, see below).
MAIN_PROFILE="dense"
# One -kvu pool shared by every slot; MAIN_SLOT_CTX is each slot's own ceiling.
MAIN_CTX=98304
MAIN_SLOT_CTX=57344
# Resident KV pool in VRAM, MiB; the rest streams from host memory. Empty drops
# block KV streaming. 1536 MiB holds ~60k tokens of the 27B resident.
MAIN_KV_POOL=1536
# Prompt cache in host RAM, MiB. It is swappable: too big pages instead of
# failing. 0 disables it, -1 is "no limit".
MAIN_CACHE_RAM=0
MAIN_NGL=99
MAIN_THREADS=8
# auto thinks unless a request sends reasoning_effort "none". -1 = no budget.
MAIN_REASONING="auto"
MAIN_REASONING_BUDGET=-1
MAIN_TEMP=1.0
MAIN_SEED=12071
# mlock keeps the weights out of swap; auto = plain mmap.
MAIN_LOAD_MODE="mlock"
# Per slot / across all slots; each hybrid checkpoint is ~157 MB.
MAIN_CTX_CHECKPOINTS=8
MAIN_CTX_CHECKPOINTS_TOTAL=6
# 1 = GGML_CUDA_ENABLE_UNIFIED_MEMORY: an oversized launch spills to host and
# pays on every decode step. The dense launch fits without it.
MAIN_CUDA_UNIFIED_MEMORY=0
# K is more sensitive than V: keep MAIN_CTK at q8_0 unless re-benchmarked.
MAIN_CTK="q8_0"
MAIN_CTV="q4_0"
# Slots. -kvu is added whenever MAIN_KV_POOL is set with more than one slot;
# slot 0 is the sidekick, slot 1 the main conversation. A fork build without
# multi-slot streaming refuses with "requires exactly one sequence (-np 1)":
# rebuild it with `ariadne doctor setup`.
MAIN_SLOTS=3
# Only for MAIN_SLOTS>1 with MAIN_KV_POOL empty.
MAIN_KV_UNIFIED=0

# --- speculative decoding (DFlash2) -----------------------------------------------
# A real second model. Empty REPO/FILE falls back to draft-mtp,ngram-mod.
# MAIN_DRAFT_FILE also takes a plain path. Q4_K_M is the floor for the drafter.
MAIN_DRAFT_REPO="Anbeeld/Qwen3.8-27B-DFlash2-GGUF"
MAIN_DRAFT_FILE="Qwen3.8-27B-DFlash2-Q6_K.gguf"
# The rollback buffer is slots x (1 + n-max) x ~150 MB: n-max 4 at 3 slots did not fit.
MAIN_DRAFT_N_MAX=2
MAIN_DRAFT_N_MIN=0
MAIN_SPEC_TYPE="draft-dflash,ngram-mod"
# ngram n-max stays under 32: a wider verify batch flips the kv-stream layout.
MAIN_NGRAM_N_MATCH=16
MAIN_NGRAM_N_MIN=16
MAIN_NGRAM_N_MAX=31
MAIN_DRAFT_CTK="q8_0"
MAIN_DRAFT_CTV="q8_0"

# --- vision ------------------------------------------------------------------------
# The projector is a separate GGUF in MAIN_REPO. MMPROJ overrides with an
# absolute path. Off by default: ~890 MB of VRAM most turns never use.
MAIN_MMPROJ_FILE="mmproj-Qwen3.8-27B-BF16.gguf"
MMPROJ=""
MAIN_VISION=0

# --- auxiliary model ----------------------------------------------------------------
# AUX_PORT == MAIN_PORT: aux is slot 0 of the main process, not its own server.
AUX_REPO="ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF"
AUX_FILE="Qwen3.8-27B-GSQ-RCO-IQ3_XXS-mtp.gguf"
AUX_PORT=8080
AUX_HOST="127.0.0.1"
AUX_CTX=12096
AUX_THREADS=6
AUX_ENABLED=1

# --- vision endpoint ---------------------------------------------------------------
# Its own CPU-only server, for vision calls that must not queue behind a turn.
VIS_ENABLED=0
VIS_REPO=""
VIS_FILE=""
VIS_MMPROJ_FILE=""
VIS_PORT=8082
VIS_HOST="127.0.0.1"
VIS_CTX=16384
VIS_THREADS=4

# --- profile a3b --------------------------------------------------------------------
# `ariadne doctor start a3b`: a 35B-A3B MoE on MAIN_PORT. Empty REPO/FILE serves
# the AFE file. Small KV, no streaming; the lever is how many layers' experts stay
# in RAM. 16 GB card, UD-Q4_K_M: n-cpu-moe 20 -> pp 402 / tg 49 tok/s, 16 -> 465 / 53.
A3B_REPO="peculiar-ragdoll/Cyber-Tiel-Coder-35B-A3B-GGUF-MTP"
A3B_FILE="Cyber-Tiel-Coder-35B-A3B-MTP-UD-Q4_K_M.gguf"
A3B_ALIAS="cybertiel-coder-35b-a3b"
A3B_CTX=32768
A3B_N_CPU_MOE=18
# -ub 512 -> 604 tok/s prefill, 2048 -> 1365; 4096 does not fit. Decode unaffected.
A3B_BATCH=2048
A3B_UBATCH=2048

# --- profile strata ------------------------------------------------------------------
# `ariadne doctor start strata`: the coder MoE through a Strata checkout.
STRATA_DIR="$HOME/projects/strata-origin-upstream"
STRATA_CONFIG="strata-coder-iq1_m.json"

# --- mazemaker AFE model --------------------------------------------------------------
# mazemaker's fact extraction calls an LLM on :8888. `ariadne doctor afe` serves
# it, only ever as the ONLY model on the card: it refuses beside main/aux/vis or
# above AFE_VRAM_BLOCK_MIB in use, and `start` refuses while it is up.
AFE_ENABLED=1
AFE_REPO="unsloth/Qwen3.6-35B-A3B-MTP-GGUF"
AFE_FILE="Qwen3.6-35B-A3B-UD-IQ3_XXS.gguf"
AFE_PORT=8888
AFE_CTX=16384
AFE_SLOTS=1
# Experts of the first N layers in RAM. N=18: 8965 MiB, ~2 GiB free beside the
# AFE window worker. N=12 (10.6 GiB) did not fit a real pass.
AFE_N_CPU_MOE=18
AFE_VRAM_BLOCK_MIB=10240
)CONF";

namespace {

const std::regex& assign_re() {
    static const std::regex re(R"(^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$)");
    return re;
}

std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}

std::string home_dir() { return env_or("HOME", ""); }

// $NAME and ${NAME}: `vars` first, then the environment; unknown names stay as written.
std::string expand(const std::string& in, const std::map<std::string, std::string>& vars) {
    std::string s = in;
    if (!s.empty() && s[0] == '~' && (s.size() == 1 || s[1] == '/')) s = home_dir() + s.substr(1);
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '$' || i + 1 >= s.size()) { out += s[i]; continue; }
        size_t j = i + 1;
        bool braced = s[j] == '{';
        if (braced) ++j;
        size_t k = j;
        while (k < s.size() && (std::isalnum(static_cast<unsigned char>(s[k])) || s[k] == '_')) ++k;
        if (k == j || (braced && (k >= s.size() || s[k] != '}'))) { out += s[i]; continue; }
        const std::string name = s.substr(j, k - j);
        const auto it = vars.find(name);
        const char* env = std::getenv(name.c_str());
        if (it != vars.end()) out += it->second;
        else if (env) out += env;
        else { out += s.substr(i, k - i + (braced ? 1 : 0)); }
        i = k - (braced ? 0 : 1);
    }
    return out;
}

std::string read_file(const fs::path& p, bool* ok = nullptr) {
    std::ifstream in(p, std::ios::binary);
    if (ok) *ok = static_cast<bool>(in);
    if (!in) return "";
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_file(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

}  // namespace

Values parse_conf(const std::string& text, const std::map<std::string, std::string>& vars) {
    Values values;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = trim(raw);
        if (line.empty() || line[0] == '#') continue;
        std::smatch m;
        if (!std::regex_match(line, m, assign_re())) continue;
        std::string rhs = trim(m[2].str());
        if (!rhs.empty() && (rhs[0] == '"' || rhs[0] == '\'')) {
            const auto end = rhs.find(rhs[0], 1);
            rhs = end != std::string::npos ? rhs.substr(1, end - 1) : rhs.substr(1);
        } else {
            rhs = trim(rhs.substr(0, rhs.find('#')));
        }
        values[m[1].str()] = expand(rhs, vars);
    }
    return values;
}

std::string StackConf::str(const std::string& key, const std::string& def) const {
    const auto it = v_.find(key);
    return (it == v_.end() || it->second.empty()) ? def : it->second;
}

long StackConf::integer(const std::string& key, long def) const {
    const std::string s = str(key, "");
    if (s.empty()) return def;
    char* end = nullptr;
    const long v = std::strtol(s.c_str(), &end, 10);
    return (end && *end == '\0') ? v : def;
}

bool StackConf::flag(const std::string& key, bool def) const { return str(key, def ? "1" : "0") == "1"; }

fs::path StackConf::llama_dir() const {
    return fs::path(str("LLAMA_DIR", home_dir() + "/projects/llama.cpp-adaptive-kv-streaming"));
}

std::pair<std::string, int> StackConf::host_port(const std::string& name) const {
    std::string prefix;
    for (char c : name) prefix += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    const int def = name == "aux" ? 8081 : name == "vis" ? 8082 : 8080;
    return {str(prefix + "_HOST", "127.0.0.1"), static_cast<int>(integer(prefix + "_PORT", def))};
}

bool StackConf::enabled(const std::string& name) const {
    if (name == "aux") return flag("AUX_ENABLED", true);
    if (name == "vis") return flag("VIS_ENABLED", false);
    return true;
}

fs::path hf_hub_dir() {
    if (const char* c = std::getenv("HF_HUB_CACHE"); c && *c) return c;
    if (const char* h = std::getenv("HF_HOME"); h && *h) return fs::path(h) / "hub";
    return fs::path(home_dir()) / ".cache/huggingface/hub";
}

fs::path local_model_file(const std::string& repo, const std::string& file, const Paths& paths) {
    std::string dir = repo;
    for (size_t p = dir.find('/'); p != std::string::npos; p = dir.find('/', p + 2)) dir.replace(p, 1, "--");
    return paths.models() / dir / file;
}

std::string model_path(const std::string& repo, const std::string& file, const Paths& paths) {
    if (file.empty()) return "";
    std::error_code ec;
    if (file[0] == '/' || file[0] == '~' || file.rfind("./", 0) == 0) {
        const fs::path p = expand(file, {});
        return fs::is_regular_file(p, ec) ? p.string() : "";
    }
    if (repo.empty()) return "";
    std::string dir = "models--" + repo;
    for (size_t p = dir.find('/'); p != std::string::npos; p = dir.find('/', p + 2)) dir.replace(p, 1, "--");
    const fs::path root = hf_hub_dir() / dir;
    if (fs::is_directory(root, ec)) {
        fs::recursive_directory_iterator it(root, fs::directory_options::follow_directory_symlink
                                                      | fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (it->path().filename() == file && fs::is_regular_file(it->path(), ec)) return it->path().string();
        }
    }
    const fs::path local = local_model_file(repo, file, paths);
    return fs::is_regular_file(local, ec) ? local.string() : "";
}

StackConf load_conf(const Paths& paths, bool create, std::string* note) {
    const std::map<std::string, std::string> vars{{"ARIADNE_HOME", paths.home.string()}};
    std::error_code ec;
    const fs::path target = paths.conf();
    if (!fs::exists(target, ec)) {
        const fs::path legacy = fs::path(home_dir()) / ".daedalus" / "stack.conf";
        if (!create) {
            bool ok = false;
            const std::string old = read_file(legacy, &ok);
            return StackConf(parse_conf(ok ? old : kDefaultConf, vars));
        }
        bool ok = false;
        const std::string old = read_file(legacy, &ok);
        if (ok && write_file(target, old)) {
            if (note) *note = "stack.conf imported from " + legacy.string();
        } else if (write_file(target, kDefaultConf)) {
            if (note) *note = "stack.conf created with defaults";
        }
    }
    if (create) {
        fs::create_directories(paths.run(), ec);
        fs::create_directories(paths.logs(), ec);
    }
    bool ok = false;
    std::string text = read_file(target, &ok);
    return StackConf(parse_conf(ok ? text : kDefaultConf, vars));
}

int physical_cores(const std::string& cpuinfo, int fallback) {
    std::set<std::pair<std::string, std::string>> pairs;
    std::map<std::string, int> per_pkg;
    std::string phys, core, pkg;
    std::istringstream in(cpuinfo);
    std::string line;
    auto value = [](const std::string& l) { const auto c = l.find(':'); return c == std::string::npos ? "" : trim(l.substr(c + 1)); };
    while (std::getline(in, line)) {
        if (line.rfind("physical id", 0) == 0) { phys = value(line); pkg = phys; }
        else if (line.rfind("core id", 0) == 0) {
            core = value(line);
            if (!phys.empty()) pairs.insert({phys, core});
        } else if (line.rfind("cpu cores", 0) == 0 && !pkg.empty()) {
            per_pkg[pkg] = std::atoi(value(line).c_str());
        }
    }
    if (!pairs.empty()) return static_cast<int>(pairs.size());
    int sum = 0;
    for (const auto& [_, n] : per_pkg) sum += n;
    return sum > 0 ? sum : (fallback > 0 ? fallback : 1);
}

std::tuple<int, int, int> propose_threads(int physical) {
    if (physical <= 0) return {1, 1, 1};
    int m, a, v;
    if (physical <= 4) { m = std::max(2, physical - 2); a = 1; v = 1; }
    else if (physical <= 8) { m = std::max(2, physical - 4); a = physical >= 6 ? 2 : 1; v = physical >= 8 ? 2 : 1; }
    else { m = physical / 2; a = std::max(2, physical / 4); v = std::max(2, physical / 4); }
    while (m + a + v > physical && m > 1) --m;
    return {m, a, v};
}

bool patch_threads(std::string& text, int main_t, int aux_t, int vis_t) {
    static const std::regex key_re(R"(^(\s*)(MAIN|AUX|VIS)_THREADS\s*=\s*(\d+)\s*(#.*)?$)");
    const std::map<std::string, int> wanted{{"MAIN", main_t}, {"AUX", aux_t}, {"VIS", vis_t}};
    std::istringstream in(text);
    std::vector<std::string> out;
    std::set<std::string> seen;
    bool changed = false;
    std::string line;
    while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_match(line, m, key_re)) { out.push_back(line); continue; }
        const std::string who = m[2].str();
        seen.insert(who);
        const int want = wanted.at(who);
        if (std::atoi(m[3].str().c_str()) == want) { out.push_back(line); continue; }
        std::string rebuilt = m[1].str() + who + "_THREADS=" + std::to_string(want);
        if (m[4].matched) rebuilt += "  " + trim(m[4].str());
        out.push_back(rebuilt);
        changed = true;
    }
    for (const auto& [who, n] : wanted) {
        if (!seen.count(who)) { out.push_back(who + "_THREADS=" + std::to_string(n)); changed = true; }
    }
    if (!changed) return false;
    std::string joined;
    for (const auto& l : out) joined += l + "\n";
    text = joined;
    return true;
}

namespace {

// Bounded GGUF reader: every read is checked, so a truncated file ends the scan.
struct Gguf {
    std::ifstream in;
    bool ok = true;
    template <typename T> T get() {
        T v{};
        if (!in.read(reinterpret_cast<char*>(&v), sizeof v)) ok = false;
        return v;
    }
    std::string str() {
        const uint64_t n = get<uint64_t>();
        if (!ok || n > (1u << 24)) { ok = false; return ""; }
        std::string s(static_cast<size_t>(n), '\0');
        if (!in.read(s.data(), static_cast<std::streamsize>(n))) ok = false;
        return s;
    }
    // The value as text for scalars; arrays and unknown types are skipped (or end the scan).
    std::string value(uint32_t kind) {
        switch (kind) {
            case 0: return std::to_string(get<uint8_t>());
            case 1: return std::to_string(get<int8_t>());
            case 2: return std::to_string(get<uint16_t>());
            case 3: return std::to_string(get<int16_t>());
            case 4: return std::to_string(get<uint32_t>());
            case 5: return std::to_string(get<int32_t>());
            case 6: return std::to_string(get<float>());
            case 7: return get<uint8_t>() ? "true" : "false";
            case 8: return str();
            case 9: {
                const uint32_t elem = get<uint32_t>();
                const uint64_t count = get<uint64_t>();
                for (uint64_t i = 0; ok && i < count; ++i) (void)value(elem);
                return "";
            }
            case 10: return std::to_string(get<uint64_t>());
            case 11: return std::to_string(get<int64_t>());
            case 12: return std::to_string(get<double>());
            default: ok = false; return "";
        }
    }
};

}  // namespace

std::string gguf_value(const std::string& path, const std::string& key) {
    Gguf g;
    g.in.open(path, std::ios::binary);
    if (!g.in) return "";
    char magic[4] = {};
    if (!g.in.read(magic, 4) || std::memcmp(magic, "GGUF", 4) != 0) return "";
    (void)g.get<uint32_t>();
    (void)g.get<uint64_t>();
    const uint64_t n_kv = g.get<uint64_t>();
    for (uint64_t i = 0; g.ok && i < n_kv; ++i) {
        const std::string k = g.str();
        const uint32_t kind = g.get<uint32_t>();
        const std::string v = g.value(kind);
        if (g.ok && k == key) return v;
    }
    return "";
}

std::string human_size(const std::string& path) {
    std::error_code ec;
    double size = static_cast<double>(fs::file_size(path, ec));
    if (ec) return "?";
    const char* units[] = {"B", "K", "M", "G", "T"};
    for (int u = 0; u < 5; ++u) {
        if (size < 1024 || u == 4) {
            char buf[32];
            std::snprintf(buf, sizeof buf, u < 2 ? "%.0f%s" : "%.1f%s", size, units[u]);
            return buf;
        }
        size /= 1024;
    }
    return "?";
}

}  // namespace doctor
}  // namespace dd
