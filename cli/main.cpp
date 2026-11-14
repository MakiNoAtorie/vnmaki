#include <cstddef>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cli/build_command.hpp"
#include "cli/check_command.hpp"
#include "cli/debug_command.hpp"
#include "cli/extract_command.hpp"
#include "cli/graph_command.hpp"
#include "cli/import_command.hpp"
#include "cli/launcher.hpp"
#include "cli/new_command.hpp"
#include "cli/run_command.hpp"
#include "cli/window_command.hpp"
#include "engine/core/no_crt_dialogs.hpp"
#include "engine/core/version.hpp"
#include "engine/platform/shell.hpp"

namespace {

constexpr int exit_usage_error = 2;

constexpr std::string_view usage = "usage: vnmaki --version\n";

}  // namespace

int main(int argc, char* argv[]) {
  vnmaki::disable_crt_dialogs();
  // Arguments are UTF-8 everywhere in the engine; argv is in the ANSI code page on Windows.
  const std::vector<std::string> utf8_line = vnmaki::command_line_utf8();
  std::vector<std::string_view> args;
  if (!utf8_line.empty()) {
    args.assign(utf8_line.begin() + 1, utf8_line.end());  // [0] is the program name
  } else {
    // argv[0] is the program name; argc can be 0 when exec'd with an empty argv.
    const std::span<char*> raw_args{argv, static_cast<std::size_t>(argc)};
    const std::span<char*> user_args = raw_args.empty() ? raw_args : raw_args.subspan(1);
    args.assign(user_args.begin(), user_args.end());
  }

  if (args.size() == 1 && args.front() == "--version") {
    std::cout << "vnmaki " << vnmaki::engine_version_string() << '\n';
    return 0;
  }

  if (!args.empty() && args.front() == "run") {
    return vnmaki::cli::run_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "new") {
    return vnmaki::cli::new_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "graph") {
    return vnmaki::cli::graph_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "check") {
    return vnmaki::cli::check_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "debug") {
    return vnmaki::cli::debug_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "extract-strings") {
    return vnmaki::cli::extract_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (!args.empty() && args.front() == "import") {
    return vnmaki::cli::import_command(std::span{args}.subspan(1), std::cout, std::cerr);
  }

  if (const std::optional<int> code =
          vnmaki::cli::build_or_packed_command(args, std::cout, std::cerr)) {
    return *code;
  }

  // A packed game (above) has priority. Without arguments, a dev build opens the launcher; a
  // release build prints the usage.
  if (!args.empty() && args.front() == "launcher") {
    return vnmaki::cli::launcher_command(std::span{args}.subspan(1), std::cerr);
  }
#if VNMAKI_DEV_TOOLS
  if (args.empty()) {
    return vnmaki::cli::launcher_command(args, std::cerr);
  }
#endif

  if (!args.empty() && args.front() == "window") {
    return vnmaki::cli::window_command(std::span{args}.subspan(1), std::cerr);
  }

  if (!args.empty()) {
    std::cerr << "vnmaki: unknown option '" << args.front() << "'\n";
  }
  std::cerr << usage << vnmaki::cli::kNewUsage << vnmaki::cli::kRunUsage
            << vnmaki::cli::kWindowUsage << vnmaki::cli::kGraphUsage << vnmaki::cli::kCheckUsage
            << vnmaki::cli::kDebugUsage << vnmaki::cli::kExtractUsage << vnmaki::cli::kImportUsage
            << vnmaki::cli::kBuildUsage << vnmaki::cli::kLauncherUsage;
  return exit_usage_error;
}
