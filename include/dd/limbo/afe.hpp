#pragma once
// dd/limbo/afe.hpp — mazemaker's AFE model on :8888, owned by limbo like every model.
//
// mazemaker's fact extraction (AFE Stage C) calls an LLM on the host. The pod cannot run host
// commands: it drops ~/.mazemaker/sockets/llm-on-demand.request, and a systemd path unit calls
// `ariadne limbo afe wake`. A refusal or a failed load is written to llm-on-demand.failed, so
// the caller in the pod fails at once with the reason instead of polling a port that will
// never open. The idle timer calls `afe idle-check`.
//
// Never two models: AFE does not start while main/aux/vis serve, or when the card already has
// more than AFE_VRAM_BLOCK_MIB in use; `ariadne limbo start` refuses while AFE serves.

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "dd/limbo/stack.hpp"

namespace dd {
namespace limbo {
namespace afe {

constexpr const char* kName = "afe";
constexpr const char* kUnit = "ariadne-afe";

std::string get(const StackConf& c, const std::string& key);   // with the AFE defaults
long num(const StackConf& c, const std::string& key);
bool enabled(const StackConf& c);
std::pair<std::string, int> host_port(const StackConf& c);
std::string probe_host(const StackConf& c);                     // 0.0.0.0 -> 127.0.0.1
std::string model(const StackConf& c, const Paths& p);

std::filesystem::path sockets_dir();                            // $MAZEMAKER_DIR/sockets
std::filesystem::path request_file();
std::filesystem::path failed_file();
std::filesystem::path last_used_file();
void report_failure(const std::string& reason);
void clear_failure();
// [llm] api_key from ~/.mazemaker/llm.toml, or "".
std::string api_key();
// llm.toml copied into sockets/, which every pod member mounts.
void share_key_with_pod();
long vram_used_mib();                                           // -1 = unknown

std::vector<std::string> stack_serving(const Stack& s, const StackConf& c);
bool serving(const Stack& s, const StackConf& c);
std::vector<std::string> blockers(const Stack& s, const StackConf& c);
std::vector<std::string> argv(const StackConf& c, const std::string& gguf, const std::string& key);
bool healthy(const StackConf& c);
// (ok, reason). Never a second model; never started blind.
std::pair<bool, std::string> start(const Stack& s, const StackConf& c, int ready_timeout = -1);

int cmd_status(const Stack& s, const StackConf* c = nullptr);
int cmd_fetch(const Stack& s);
int cmd_start(const Stack& s);
int cmd_stop(const Stack& s);
int cmd_wake(const Stack& s);
int cmd_idle_check(const Stack& s);

}  // namespace afe
}  // namespace limbo
}  // namespace dd
