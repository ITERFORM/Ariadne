#pragma once
// dd/doctor/doctor.hpp — `ariadne doctor`: the local inference stack and mazemaker's AFE model.

#include <filesystem>
#include <string>
#include <vector>

namespace dd {
namespace doctor {

// args are what follows `doctor` on the command line; `home` is the resolved ariadne home.
int run_doctor(const std::vector<std::string>& args, const std::filesystem::path& home);
void doctor_usage(bool to_stderr);

}  // namespace doctor
}  // namespace dd
