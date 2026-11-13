#include "cli/run_command.hpp"

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "cli/story_window.hpp"
#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/saves/saves.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;
constexpr std::size_t kDefaultMaxSteps = 100'000;

struct RunArguments {
  std::filesystem::path project_dir = ".";
  bool headless = false;
  std::size_t max_steps = kDefaultMaxSteps;
  std::optional<std::size_t> max_frames;
  std::optional<std::filesystem::path> data_dir;
  std::optional<std::filesystem::path> profile_csv;
};

std::optional<std::size_t> parse_positive(std::string_view text) {
  std::size_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc{} || end != text.data() + text.size() || value == 0) {
    return std::nullopt;
  }
  return value;
}

// Parses the arguments after "run", or prints why not and returns nothing.
std::optional<RunArguments> parse_arguments(std::span<const std::string_view> args,
                                            std::ostream& err) {
  RunArguments parsed;
  bool have_project_dir = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--headless") {
      parsed.headless = true;
    } else if (arg == "--max-steps") {
      const std::optional<std::size_t> steps =
          i + 1 < args.size() ? parse_positive(args[i + 1]) : std::nullopt;
      if (!steps) {
        err << "vnmaki run: --max-steps needs a positive whole number\n";
        return std::nullopt;
      }
      parsed.max_steps = *steps;
      ++i;
    } else if (arg == "--data-dir") {
      if (i + 1 >= args.size() || args[i + 1].empty()) {
        err << "vnmaki run: --data-dir needs a folder\n";
        return std::nullopt;
      }
      parsed.data_dir = std::filesystem::path{args[i + 1]};
      ++i;
    } else if (arg == "--profile") {
      if (i + 1 >= args.size() || args[i + 1].empty()) {
        err << "vnmaki run: --profile needs a file\n";
        return std::nullopt;
      }
      parsed.profile_csv = std::filesystem::path{args[i + 1]};
      ++i;
    } else if (arg == "--frames") {
      parsed.max_frames = i + 1 < args.size() ? parse_positive(args[i + 1]) : std::nullopt;
      if (!parsed.max_frames) {
        err << "vnmaki run: --frames needs a positive whole number\n";
        return std::nullopt;
      }
      ++i;
    } else if (arg.starts_with("-") || have_project_dir) {
      err << "vnmaki run: unexpected argument '" << arg << "'\n";
      return std::nullopt;
    } else {
      parsed.project_dir = std::filesystem::path{arg};
      have_project_dir = true;
    }
  }
  if (parsed.headless && parsed.max_frames) {
    err << "vnmaki run: --frames needs a window, so it cannot go with --headless\n";
    return std::nullopt;
  }
  if (parsed.headless && parsed.profile_csv) {
    err << "vnmaki run: --profile needs a window, so it cannot go with --headless\n";
    return std::nullopt;
  }
  return parsed;
}

}  // namespace

int run_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  const std::optional<RunArguments> arguments = parse_arguments(args, err);
  if (!arguments) {
    err << "usage:" << kRunUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }

  Vfs vfs;
  if (!vfs.mount(make_directory_mount(arguments->project_dir))) {
    err << "vnmaki: " << arguments->project_dir << " is not a directory\n";
    return kExitError;
  }
  ProjectConfig config;
  try {
    config = load_project_config(vfs);
  } catch (const ProjectConfigError& error) {
    err << "vnmaki: " << error.what() << '\n';
    return kExitError;
  }

  if (!arguments->headless) {
    return run_story_window(vfs, config, arguments->max_frames, arguments->data_dir, err,
                            arguments->profile_csv);
  }

  // Nothing is drawn or heard, but the scene and the audio still check every file a script
  // names. The audio has no device: no time passes in a headless run.
  const Size resolution{.width = config.resolution.width, .height = config.resolution.height};
  Scene scene{vfs, resolution};
  Audio audio{vfs};
  CompositeSceneState state{{&scene, &audio}};
  HeadlessOptions options;
  options.runtime.rollback_depth = config.rollback_depth;
  options.runtime.gallery = config.gallery;
  options.runtime.scene = &scene;
  options.runtime.audio = &audio;
  options.runtime.scene_state = &state;
  // The scripts' `save` and `load` work, and `persistent` is kept, in the same directory as in
  // a window (or in --data-dir). Without a user-data directory the run goes on without them.
  try {
    options.saves =
        SavesOptions{.directory = arguments->data_dir ? *arguments->data_dir
                                                      : user_data_directory("VNMaki", config.title),
                     .game_version = config.version,
                     .auto_save = config.auto_save};
  } catch (const PlatformError& error) {
    err << "vnmaki: " << error.what() << "; saves and persistent data are off\n";
  }
  const std::vector<ScriptInput> inputs(arguments->max_steps, AdvanceInput{});
  const HeadlessResult result = run_headless(vfs, inputs, options);
  for (const std::string& line : result.transcript) {
    out << line << '\n';
  }
  out.flush();
  switch (result.outcome) {
    case HeadlessOutcome::kFinished:
      return 0;
    case HeadlessOutcome::kOutOfInput:
      err << "vnmaki: stopped after " << arguments->max_steps
          << " steps (--max-steps) before the story ended\n";
      return kExitError;
    case HeadlessOutcome::kFailed:
      break;
  }
  err << "vnmaki: " << result.error << '\n';
  return kExitError;
}

}  // namespace vnmaki::cli
