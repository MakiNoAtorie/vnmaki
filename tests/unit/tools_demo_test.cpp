// The Phase 4 tools demo (docs/PLAN.md task 37), part 1: hot reload on the default template.
// The template is made by `vnmaki new` into a temporary folder, played to a line of the story, and
// one say line is changed on disk. No window or GPU is needed. The DAP session, `vnmaki check`,
// `vnmaki graph` and the profile CSV of the demo are CLI tests (tests/cli/CMakeLists.txt).

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/runtime/new_project.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/file_watcher.hpp"
#include "engine/tools/hot_reload.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kScript = "scripts/main.lua";
constexpr std::string_view kOldLine = "I'm glad. I kept your seat by the window.";
constexpr std::string_view kNewLine = "I am very glad. The seat by the window is yours.";
constexpr std::chrono::seconds kWatchTimeout{10};
constexpr std::chrono::milliseconds kPollStep{10};

struct TempProject {
  explicit TempProject(const std::string& name)
      : root(vnmaki::test::unique_temp_path("vnmaki_tools_demo_", name)) {
    fs::remove_all(root);
  }
  TempProject(const TempProject&) = delete;
  TempProject& operator=(const TempProject&) = delete;
  ~TempProject() {
    std::error_code ignored;  // a folder left in the temp directory is not worth a failure
    fs::remove_all(root, ignored);
  }

  fs::path root;
};

std::string read_file(const fs::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, std::string_view text) {
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  stream << text;
}

// The template as `vnmaki run` plays it, with hot reload on top, at its `start` label.
class TemplateGame {
 public:
  explicit TemplateGame(const std::string& name)
      : temp_(name),
        project_(temp_.root / "my_game"),
        config_(prepare()),
        scene_(vfs_, {.width = config_.resolution.width, .height = config_.resolution.height}),
        audio_(vfs_),
        state_({&scene_, &audio_}),
        runtime_(vfs_, {.rollback_depth = config_.rollback_depth,
                        .scene_state = &state_,
                        .scene = &scene_,
                        .audio = &audio_}),
        ui_({.width = config_.resolution.width, .height = config_.resolution.height}),
        hot_(runtime_, scene_, jobs_, ui_, log_) {
    runtime_.load_scripts();
    runtime_.start("start");
  }

  // Closes the title screen, ends the dissolve, and answers the first menu option: the story is
  // then at the first line of `warm`.
  void play_to_warm() {
    REQUIRE(std::holds_alternative<vnmaki::CallScreenRequest>(*runtime_.request()));
    runtime_.return_screen(vnmaki::ScreenValue{std::string{"start"}});
    settle();
    runtime_.advance();  // "You came back."
    settle();
    REQUIRE(std::holds_alternative<vnmaki::MenuRequest>(*runtime_.request()));
    runtime_.choose(0);
    settle();
  }

  [[nodiscard]] std::string say_text() const {
    const vnmaki::BlockingRequest* request = runtime_.request();
    REQUIRE(request != nullptr);
    const auto* say = std::get_if<vnmaki::SayRequest>(request);
    REQUIRE(say != nullptr);
    return say->text;
  }

  // Edits the script on disk and waits for the file watcher to report it, as the story window
  // does it. Where the watcher is not used, the batch is made by hand.
  void edit_script(const std::string& from, const std::string& to) {
    const fs::path file = project_ / kScript;
    std::string text = read_file(file);
    const std::size_t at = text.find(from);
    REQUIRE(at != std::string::npos);
    text.replace(at, from.size(), to);
#ifdef _WIN32
    vnmaki::FileWatcher watcher{project_};
    write_file(file, text);
    hot_.apply(wait_for_batch(watcher));
#else
    write_file(file, text);
    hot_.apply(std::vector<std::string>{std::string{kScript}});
#endif
  }

  [[nodiscard]] vnmaki::ScriptRuntime& runtime() { return runtime_; }
  [[nodiscard]] const vnmaki::HotReload& hot() const { return hot_; }
  [[nodiscard]] const vnmaki::LogBuffer& log() const { return log_; }

 private:
  // `vnmaki new`, then mounts the folder as `vnmaki run` does. vfs_ is made before config_.
  vnmaki::ProjectConfig prepare() {
    vnmaki::create_project(fs::path{VNMAKI_TEMPLATES_DIR} / "default", project_, "my_game");
    REQUIRE(vfs_.mount(vnmaki::make_directory_mount(project_)));
    return vnmaki::load_project_config(vfs_);
  }

  // The first batch that contains the script; an empty batch is the timeout.
  static std::vector<std::string> wait_for_batch(vnmaki::FileWatcher& watcher) {
    const auto deadline = vnmaki::FileWatcher::Clock::now() + kWatchTimeout;
    while (vnmaki::FileWatcher::Clock::now() < deadline) {
      std::vector<std::string> batch = watcher.poll();
      if (!batch.empty()) {
        return batch;
      }
      std::this_thread::sleep_for(kPollStep);
    }
    return {};
  }

  // Ends the waits of the story, as the headless runner does; a movie is not played here.
  void settle() {
    while (const vnmaki::BlockingRequest* request = runtime_.request()) {
      if (const auto* wait = std::get_if<vnmaki::WaitRequest>(request)) {
        scene_.finish_tweens(wait->tag);
      } else if (!std::holds_alternative<vnmaki::MovieRequest>(*request)) {
        return;
      }
      runtime_.advance();
    }
  }

  TempProject temp_;
  fs::path project_;
  vnmaki::Vfs vfs_;
  vnmaki::ProjectConfig config_;
  vnmaki::Scene scene_;
  vnmaki::Audio audio_;
  vnmaki::CompositeSceneState state_;
  vnmaki::ScriptRuntime runtime_;
  vnmaki::JobSystem jobs_{0};
  vnmaki::UiHost ui_;
  vnmaki::LogBuffer log_;
  vnmaki::HotReload hot_;
};

}  // namespace

TEST_CASE("tools demo: a say line of the template changes on disk and shows at the same step") {
  TemplateGame game{"say"};
  game.play_to_warm();
  REQUIRE(game.say_text() == kOldLine);
  const vnmaki::ScriptPosition before = game.runtime().position();
  const std::size_t steps = game.runtime().rollback_steps().size();
  REQUIRE(before.label == "warm");

  game.edit_script(std::string{kOldLine}, std::string{kNewLine});

  CHECK(game.say_text() == kNewLine);
  CHECK(game.runtime().position().label == before.label);
  CHECK(game.runtime().position().step == before.step);
  CHECK(game.runtime().rollback_steps().size() == steps);
  CHECK(game.hot().banner().empty());
  for (const vnmaki::LogEntry& entry : game.log().entries()) {
    CHECK(entry.level != vnmaki::LogLevel::kError);
  }
  game.runtime().advance();  // the story goes on with the next line
  CHECK(game.say_text() == "Let's walk home together.");
}
