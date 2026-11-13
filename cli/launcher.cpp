#include "cli/launcher.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "cli/build_command.hpp"
#include "cli/check_command.hpp"
#include "cli/extract_command.hpp"
#include "cli/new_command.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/platform/shell.hpp"

namespace vnmaki::cli {

namespace fs = std::filesystem;

namespace {

constexpr int kExitFailure = 1;
constexpr int kExitUsage = 2;
constexpr std::string_view kRecentFileName = "recent.txt";

using Command = int (*)(std::span<const std::string_view>, std::ostream&, std::ostream&);

// What the launcher writes of the command line, to tell the log lines apart.
std::string command_line(std::string_view name, std::span<const std::string_view> args) {
  std::string line = "> vnmaki " + std::string{name};
  for (const std::string_view arg : args) {
    line += " \"" + std::string{arg} + '"';
  }
  return line + '\n';
}

}  // namespace

RecentProjects::RecentProjects(fs::path file) : file_(std::move(file)) {
  std::ifstream stream{file_};
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (!line.empty() && projects_.size() < kMaxProjects) {
      projects_.emplace_back(line);
    }
  }
}

bool RecentProjects::add(const fs::path& folder) {
  std::error_code error;
  fs::path entry = fs::absolute(folder, error).lexically_normal();
  if (!entry.has_filename()) {
    entry = entry.parent_path();  // "a/b/" is the folder "a/b"
  }
  std::erase(projects_, entry);
  projects_.insert(projects_.begin(), entry);
  if (projects_.size() > kMaxProjects) {
    projects_.resize(kMaxProjects);
  }
  if (!file_.parent_path().empty()) {
    fs::create_directories(file_.parent_path(), error);
  }
  std::ofstream stream{file_, std::ios::trunc};
  for (const fs::path& project : projects_) {
    stream << project.string() << '\n';
  }
  return static_cast<bool>(stream.flush());
}

LauncherHost system_launcher_host() {
  return {.spawn_vnmaki =
              [](std::span<const fs::path> args) { return spawn_process(executable_path(), args); },
          .open_folder = [](const fs::path& folder) { return vnmaki::open_folder(folder); },
          .runner = std::nullopt};
}

int Launcher::run(LauncherAction action, std::string_view folder) {
  if (folder.empty()) {
    log_ += "Enter a project folder first.\n";
    return kExitUsage;
  }
  const std::string dir{folder};
  const std::string build_folder = (fs::path{dir} / ".vnmaki" / "windows").string();
  const std::string runner = host_.runner ? host_.runner->string() : std::string{};
  std::ostringstream text;  // out and err together, so that the log keeps their order
  int code = 0;
  try {
    const auto call = [&](std::string_view name, Command command,
                          std::span<const std::string_view> args) {
      text << command_line(name, args);
      return command(args, text, text);
    };
    switch (action) {
      case LauncherAction::kNew: {
        const std::string_view args[] = {dir};
        code = call("new", new_command, args);
        break;
      }
      case LauncherAction::kCheck: {
        const std::string_view args[] = {dir};
        code = call("check", check_command, args);
        break;
      }
      case LauncherAction::kExtractStrings: {
        const std::string_view args[] = {dir};
        code = call("extract-strings", extract_command, args);
        break;
      }
      case LauncherAction::kBuildWindows: {
        std::vector<std::string_view> args = {"windows", dir, "--out", build_folder};
        if (host_.runner) {
          args.insert(args.end(), {"--runner", runner});
        }
        code = call("build", build_command, args);
        break;
      }
      case LauncherAction::kRun: {
        const std::string_view shown[] = {dir};
        text << command_line("run", shown);
        const fs::path args[] = {"run", fs::path{dir}};
        code = host_.spawn_vnmaki(args) ? 0 : kExitFailure;
        text << (code == 0 ? "Started the game in its own window.\n" : "Cannot start vnmaki.\n");
        break;
      }
      case LauncherAction::kOpenFolder:
        code = host_.open_folder(fs::path{dir}) ? 0 : kExitFailure;
        if (code != 0) {
          text << "Cannot open \"" << dir << "\".\n";
        }
        break;
    }
  } catch (const std::exception& error) {
    text << "Error: " << error.what() << '\n';
    code = kExitFailure;
  }
  log_ += text.str();
  if (code == 0 && !recent_.add(fs::path{dir})) {
    log_ += "Cannot write the list of recent projects.\n";
  }
  return code;
}

int launcher_command(std::span<const std::string_view> args, std::ostream& err) {
  std::optional<std::uint64_t> max_frames;
  std::optional<fs::path> data_directory;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if (args[i] == "--frames" && i + 1 < args.size()) {
      const std::string_view count = args[++i];
      std::uint64_t frames = 0;
      const auto parsed = std::from_chars(count.data(), count.data() + count.size(), frames);
      if (parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size()) {
        err << "vnmaki launcher: --frames needs a whole number, not '" << count << "'\n";
        return kExitUsage;
      }
      max_frames = frames;
    } else if (args[i] == "--data-dir" && i + 1 < args.size()) {
      data_directory = fs::path{args[++i]};
    } else {
      err << "vnmaki launcher: unexpected argument '" << args[i] << "'\nusage:"
          << kLauncherUsage.substr(std::string_view{"      "}.size());
      return kExitUsage;
    }
  }
#if VNMAKI_DEV_TOOLS
  try {
    const fs::path folder =
        data_directory ? *data_directory : user_data_directory("VNMaki", "launcher");
    Launcher launcher{RecentProjects{folder / kRecentFileName}, system_launcher_host()};
    return run_launcher_window(launcher, max_frames, err);
  } catch (const PlatformError& error) {
    err << "vnmaki launcher: " << error.what() << '\n';
    return kExitFailure;
  }
#else
  static_cast<void>(max_frames);
  static_cast<void>(data_directory);
  err << "vnmaki launcher: this build has no launcher\n";
  return kExitFailure;
#endif
}

}  // namespace vnmaki::cli
