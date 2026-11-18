#pragma once
// dd/doctor/stack_conf.hpp — stack.conf: the local inference stack's knobs.
//
// Shell syntax because it predates this port and is read by hand, but parsed, never sourced:
// running the doctor must never execute whatever a conf file happens to contain. It lives in
// the ariadne home; a conf the Python doctor left in ~/.daedalus is imported once, so a
// machine's measured numbers survive the move.

#include <filesystem>
#include <map>
#include <string>
#include <tuple>
#include <utility>

namespace dd {
namespace doctor {

using Values = std::map<std::string, std::string>;

// The annotated default written on first use.
extern const char* const kDefaultConf;

// KEY=value lines: optional quotes, ~, $HOME, ${VAR} and the names in `vars` expanded; a
// trailing # comment on an unquoted value dropped. An unknown $VAR stays as written.
Values parse_conf(const std::string& text, const std::map<std::string, std::string>& vars);

class StackConf {
public:
    StackConf() = default;
    explicit StackConf(Values v) : v_(std::move(v)) {}

    // Empty counts as unset: a key written with no value takes the default.
    std::string str(const std::string& key, const std::string& def = "") const;
    long integer(const std::string& key, long def) const;
    bool flag(const std::string& key, bool def) const;

    std::filesystem::path llama_dir() const;
    std::filesystem::path server() const { return llama_dir() / "build/bin/llama-server"; }
    // main 8080, aux 8081, vis 8082 unless the conf names a port.
    std::pair<std::string, int> host_port(const std::string& name) const;
    bool enabled(const std::string& name) const;
    const Values& values() const { return v_; }

private:
    Values v_;
};

struct Paths {
    std::filesystem::path home;   // the ariadne home
    std::filesystem::path conf() const { return home / "stack.conf"; }
    std::filesystem::path run() const { return home / "run"; }
    std::filesystem::path logs() const { return home / "logs" / "stack"; }
    std::filesystem::path slots() const { return home / "slots"; }
    std::filesystem::path models() const { return home / "models"; }   // downloads without the hf CLI
};

// HF_HUB_CACHE, else HF_HOME/hub, else ~/.cache/huggingface/hub.
std::filesystem::path hf_hub_dir();

// A GGUF on disk, or "": a plain path (/, ~, ./) as is; else inside the HF cache (snapshot
// entries are symlinks into blobs/, so the walk follows links), else under paths.models().
std::string model_path(const std::string& repo, const std::string& file, const Paths& paths);

// Where a download without the hf CLI lands: <models>/<org>--<name>/<file>.
std::filesystem::path local_model_file(const std::string& repo, const std::string& file, const Paths& paths);

// Load stack.conf. Missing: import ~/.daedalus/stack.conf when that exists, else write the
// default — unless `create` is false (read-only reports leave nothing behind). `note`
// receives one line about an import or a fresh file.
StackConf load_conf(const Paths& paths, bool create, std::string* note = nullptr);

// Distinct (physical id, core id) pairs in /proc/cpuinfo text; `fallback` when it says nothing.
int physical_cores(const std::string& cpuinfo, int fallback);
// main/aux/vis threads whose sum never passes the physical core count.
std::tuple<int, int, int> propose_threads(int physical);
// Rewrite MAIN/AUX/VIS_THREADS in place, keeping comments; append the missing ones. True if changed.
bool patch_threads(std::string& text, int main_t, int aux_t, int vis_t);

// One scalar metadata value out of a GGUF header as text, or "" on any parse trouble.
std::string gguf_value(const std::string& path, const std::string& key);
std::string human_size(const std::string& path);

}  // namespace doctor
}  // namespace dd
