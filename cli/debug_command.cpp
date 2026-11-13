#include "cli/debug_command.hpp"

#include <exception>
#include <filesystem>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>

#include "engine/runtime/project_config.hpp"
#include "engine/vfs/vfs.hpp"

#if VNMAKI_DEV_TOOLS && defined(_WIN32)
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <thread>
#include <variant>

#include <fcntl.h>
#include <io.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "cli/story_window.hpp"
#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/saves/saves.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/dap.hpp"
#include "engine/tools/debugger.hpp"
#endif

namespace vnmaki::cli {

namespace {

constexpr int kExitError = 1;
constexpr int kExitUsage = 2;

struct DebugArguments {
  std::filesystem::path project_dir = ".";
  bool headless = false;
  std::optional<std::filesystem::path> data_dir;
};

std::optional<DebugArguments> parse_arguments(std::span<const std::string_view> args,
                                              std::ostream& err) {
  DebugArguments parsed;
  bool have_project_dir = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string_view arg = args[i];
    if (arg == "--headless") {
      parsed.headless = true;
    } else if (arg == "--data-dir") {
      if (i + 1 >= args.size() || args[i + 1].empty()) {
        err << "vnmaki debug: --data-dir needs a folder\n";
        return std::nullopt;
      }
      parsed.data_dir = std::filesystem::path{args[i + 1]};
      ++i;
    } else if (arg.starts_with("-") || have_project_dir) {
      err << "vnmaki debug: unexpected argument '" << arg << "'\n";
      return std::nullopt;
    } else {
      parsed.project_dir = std::filesystem::path{arg};
      have_project_dir = true;
    }
  }
  return parsed;
}

#if VNMAKI_DEV_TOOLS && defined(_WIN32)

constexpr std::size_t kReadChunk = 4096;
constexpr std::chrono::milliseconds kIdleSleep{2};
// A story that never ends still stops a headless session after this many answers.
constexpr std::size_t kMaxHeadlessAnswers = 100'000;
constexpr std::string_view kUserDataOrganization = "VNMaki";

// DAP on the process's stdin and stdout. Reads never block: a pipe is asked how much it holds,
// and a file or a closed pipe is read at once. Writes are raw bytes (no CRLF translation).
class StdioChannel final : public DapChannel {
 public:
  StdioChannel() : input_(GetStdHandle(STD_INPUT_HANDLE)), type_(GetFileType(input_)) {
    static_cast<void>(_setmode(_fileno(stdout), _O_BINARY));
  }

  Status receive(std::string& body) override {
    while (true) {
      if (std::optional<std::string> message = framer_.next()) {
        body = std::move(*message);
        return Status::kMessage;
      }
      if (closed_) {
        return Status::kClosed;
      }
      if (!input_ready()) {
        return Status::kNothing;
      }
      read_chunk();
    }
  }

  void send(std::string_view json) override {
    const std::string framed = frame_dap_message(json);
    static_cast<void>(std::fwrite(framed.data(), 1, framed.size(), stdout));
    static_cast<void>(std::fflush(stdout));
  }

 private:
  // A pipe with nothing in it would block ReadFile. For a file (or a console, which this does not
  // support) a read answers at once or ends.
  [[nodiscard]] bool input_ready() const {
    if (type_ != FILE_TYPE_PIPE) {
      return true;
    }
    DWORD available = 0;
    if (PeekNamedPipe(input_, nullptr, 0, nullptr, &available, nullptr) == 0) {
      return true;  // the writer is gone: the read reports the end
    }
    return available > 0;
  }

  void read_chunk() {
    std::string buffer(kReadChunk, '\0');
    DWORD count = 0;
    if (ReadFile(input_, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) == 0 ||
        count == 0) {
      closed_ = true;
      return;
    }
    framer_.feed(std::string_view{buffer}.substr(0, count));
  }

  HANDLE input_;
  DWORD type_;
  DapFramer framer_;
  bool closed_ = false;
};

// The headless "player": answers the request the story waits at, the way `vnmaki run --headless`
// does (a wait ends at once, a menu takes its first option).
void answer_request(ScriptRuntime& runtime, Scene& scene) {
  const BlockingRequest* request = runtime.request();
  if (const auto* wait = std::get_if<WaitRequest>(request)) {
    scene.finish_tweens(wait->tag);
    runtime.advance();
  } else if (std::holds_alternative<MenuRequest>(*request)) {
    runtime.choose(0);
  } else if (std::holds_alternative<CallScreenRequest>(*request)) {
    runtime.return_screen(ScreenValue{});
  } else {
    runtime.advance();
  }
}

int run_headless_debug(const Vfs& vfs, const std::filesystem::path& project_dir,
                       const ProjectConfig& config,
                       const std::optional<std::filesystem::path>& data_dir, DapChannel& channel,
                       std::ostream& err) {
  const Size resolution{.width = config.resolution.width, .height = config.resolution.height};
  Scene scene{vfs, resolution};
  Audio audio{vfs};
  CompositeSceneState state{{&scene, &audio}};
  ScriptRuntime runtime{vfs,
                        {.rollback_depth = config.rollback_depth,
                         .scene_state = &state,
                         .scene = &scene,
                         .audio = &audio,
                         .gallery = config.gallery}};
  const auto warn = [&](std::string_view message) { err << "vnmaki: " << message << '\n'; };
  const std::filesystem::path directory =
      data_dir ? *data_dir : user_data_directory(kUserDataOrganization, config.title);
  PersistentStore persistent{runtime, directory, warn};
  persistent.load();
  runtime.load_scripts();
  Saves saves{runtime,
              {.directory = directory,
               .game_version = config.version,
               .auto_save = config.auto_save,
               .on_warning = warn}};

  Debugger debugger{runtime, channel, project_dir, [] {
                      std::this_thread::sleep_for(kIdleSleep);
                      return true;
                    }};
  while (!debugger.configured()) {
    if (!debugger.poll()) {
      return 0;
    }
    std::this_thread::sleep_for(kIdleSleep);
  }
  runtime.start("start");
  std::size_t answers = 0;
  while (true) {
    // The story runs on by itself until it stops, ends or waits after a goto. No request is read
    // in between, so what a request sees does not depend on timing.
    while (runtime.state() == RunState::kWaiting && !debugger.holding() &&
           answers++ < kMaxHeadlessAnswers) {
      saves.poll();
      persistent.flush();
      if (runtime.state() == RunState::kWaiting) {
        answer_request(runtime, scene);
      }
    }
    if (!debugger.poll()) {
      break;
    }
    std::this_thread::sleep_for(kIdleSleep);
  }
  if (runtime.state() == RunState::kFailed) {
    err << "vnmaki: " << runtime.error() << '\n';
    return kExitError;
  }
  return 0;
}

#endif

}  // namespace

int debug_command(std::span<const std::string_view> args, std::ostream& out, std::ostream& err) {
  static_cast<void>(out);  // the protocol owns stdout (StdioChannel)
  const std::optional<DebugArguments> arguments = parse_arguments(args, err);
  if (!arguments) {
    err << "usage:" << kDebugUsage.substr(std::string_view{"      "}.size());
    return kExitUsage;
  }
#if VNMAKI_DEV_TOOLS && defined(_WIN32)
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
  try {
    StdioChannel channel;
    if (arguments->headless) {
      return run_headless_debug(vfs, arguments->project_dir, config, arguments->data_dir, channel,
                                err);
    }
    return run_story_window(vfs, arguments->project_dir, config, std::nullopt, arguments->data_dir,
                            err, std::nullopt, &channel);
  } catch (const std::exception& error) {
    err << "vnmaki: " << error.what() << '\n';
    return kExitError;
  }
#else
  err << "vnmaki debug: this build has no debugger (it needs Windows and the dev tools)\n";
  return kExitError;
#endif
}

}  // namespace vnmaki::cli
