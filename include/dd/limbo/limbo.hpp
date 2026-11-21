#pragma once
// dd/limbo/limbo.hpp — `ariadne limbo`: the local inference stack and mazemaker's AFE model.

#include <filesystem>
#include <string>
#include <vector>

namespace dd {
namespace limbo {

// args are what follows `limbo` on the command line; `home` is the resolved ariadne home.
int run_limbo(const std::vector<std::string>& args, const std::filesystem::path& home);
void limbo_usage(bool to_stderr);

}  // namespace limbo
}  // namespace dd
