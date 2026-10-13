#include <cstddef>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

#include "cli/run_command.hpp"
#include "cli/window_command.hpp"
#include "engine/core/version.hpp"

namespace {

constexpr int exit_usage_error = 2;

constexpr std::string_view usage = "usage: vnmaki --version\n";

}  // namespace

int main(int argc, char* argv[]) {
  const std::span<char*> raw_args{argv, static_cast<std::size_t>(argc)};
  // argv[0] is the program name; argc can be 0 when exec'd with an empty argv.
  const std::span<char*> user_args = raw_args.empty() ? raw_args : raw_args.subspan(1);
  const std::vector<std::string_view> args(user_args.begin(), user_args.end());

  if (args.size() == 1 && args.front() == "--version") {
    std::cout << "vnmaki " << vnmaki::engine_version_string() << '\n';
    return 0;
  }

  if (!args.empty() && args.front() == "run") {
    return vnmaki::cli::run_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "window") {
    return vnmaki::cli::window_command(std::span{args}.subspan(1), std::cerr);
  }

  if (!args.empty()) {
    std::cerr << "vnmaki: unknown option '" << args.front() << "'\n";
  }
  std::cerr << usage << vnmaki::cli::kRunUsage << vnmaki::cli::kWindowUsage;
  return exit_usage_error;
}
