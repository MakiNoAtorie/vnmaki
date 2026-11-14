#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki::cli {

inline constexpr std::string_view kLauncherUsage =
    "       vnmaki launcher [--frames <n>] [--data-dir <path>]\n";

/// The folders that the launcher used last, newest first. They are kept in a text file with one
/// folder on each line. A folder name is read and written as UTF-8, like the arguments of the
/// command line.
class RecentProjects {
 public:
  static constexpr std::size_t kMaxProjects = 10;

  /// Reads `file`. A file that is missing or cannot be read is an empty list.
  explicit RecentProjects(std::filesystem::path file);

  [[nodiscard]] const std::vector<std::filesystem::path>& list() const { return projects_; }

  /// Puts `folder` (made absolute and normal) first, removes its older entry and the entries past
  /// kMaxProjects, and writes the file. Returns false if the file cannot be written; the list in
  /// memory is changed in any case.
  bool add(const std::filesystem::path& folder);

 private:
  std::filesystem::path file_;
  std::vector<std::filesystem::path> projects_;
};

/// What the launcher buttons do.
enum class LauncherAction { kNew, kRun, kCheck, kExtractStrings, kBuildWindows, kOpenFolder };

/// What an action asks of the OS. Tests give fakes; `system_launcher_host()` is the real one.
struct LauncherHost {
  /// Starts `vnmaki <args>` as a new process and returns at once (the Run button).
  std::function<bool(std::span<const std::filesystem::path> args)> spawn_vnmaki;
  /// Shows a folder in the file manager.
  std::function<bool(const std::filesystem::path&)> open_folder;
  /// The `--runner` of Build windows. Without it the build uses its own default runner.
  std::optional<std::filesystem::path> runner;
};

[[nodiscard]] LauncherHost system_launcher_host();

/// The state of the launcher window without ImGui: the recent projects and the log.
///
/// An action does what the CLI command of the same name does, by calling its function
/// (`new_command`, `check_command`, `extract_command`, `build_command`) on the folder, and it adds
/// what the command wrote to the log. The exceptions are Run, which starts `vnmaki run <folder>`
/// as a separate process so that the launcher stays open while the game plays (and a crash of the
/// game does not close it), and Open folder. The Build windows output folder is
/// `<folder>/.vnmaki/windows`.
class Launcher {
 public:
  Launcher(RecentProjects recent, LauncherHost host)
      : recent_(std::move(recent)), host_(std::move(host)) {}

  /// Runs `action` on `folder` (for New: the folder to make). Returns the exit code of the
  /// command: 0 on success, 1 when it fails, 2 when `folder` is empty. A folder with exit code 0
  /// goes to the recent projects. The log gets the command line `> vnmaki ...` and the output.
  /// For New, `languages` are the add-ons to apply (`--lang <code>` for each).
  int run(LauncherAction action, std::string_view folder,
          const std::vector<std::string>& languages = {});

  [[nodiscard]] const RecentProjects& recent() const { return recent_; }
  [[nodiscard]] const std::string& log() const { return log_; }
  void clear_log() { log_.clear(); }

 private:
  RecentProjects recent_;
  LauncherHost host_;
  std::string log_;
};

/// `vnmaki launcher [--frames <n>] [--data-dir <path>]` (docs/SPEC.md section 19): a window with
/// the recent projects and the buttons New, Run, Check, Extract strings, Build windows and Open
/// folder, and a log. `--frames` ends it after <n> frames; `--data-dir` is the folder of the
/// recent-project file `recent.txt` (default: the user-data directory of the application
/// "launcher"). `vnmaki` with no
/// arguments does the same in a build with dev tools. A release build has no launcher: it gives
/// exit code 1.
///
/// `args` are the arguments after "launcher". Returns the process exit code: 0, 1 when the window
/// or the GPU cannot be used, 2 on a usage error.
int launcher_command(std::span<const std::string_view> args, std::ostream& err);

#if VNMAKI_DEV_TOOLS
/// The window of `launcher` (cli/launcher_window.cpp, dev tools only). Returns 0, or 1 when the
/// window or the GPU cannot be used.
int run_launcher_window(Launcher& launcher, std::optional<std::uint64_t> max_frames,
                        std::ostream& err);
#endif

}  // namespace vnmaki::cli
