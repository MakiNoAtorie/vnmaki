// Hot reload (docs/SPEC.md section 18): batches of changed files, and what they do to a running
// game. The tests edit real files in a temporary project folder; no window or GPU is needed.

#include "engine/tools/hot_reload.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/file_watcher.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/recording_renderer.hpp"

namespace {

using std::chrono::milliseconds;
using vnmaki::BlockingRequest;
using vnmaki::NarrateRequest;
using vnmaki::RunState;
using Clock = vnmaki::FileWatcher::Clock;

constexpr vnmaki::Size kVirtualSize{.width = 64, .height = 64};
constexpr std::uint32_t kInstructionLimit = 100000;
constexpr int kImageSmall = 2;  // rgba_2x2.png
constexpr int kImageLarge = 4;  // checker_4x4.png
constexpr milliseconds kQuiet{30};

const std::filesystem::path kImages =
    std::filesystem::path{VNMAKI_FIXTURES_DIR} / "render" / "images";

std::string read_text(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  REQUIRE(file);
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

// A project folder in the temp directory that the test edits.
class TempProject {
 public:
  explicit TempProject(const std::string& name)
      : root_(std::filesystem::temp_directory_path() / ("vnmaki_hot_reload_" + name)) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_);
  }
  TempProject(const TempProject&) = delete;
  TempProject& operator=(const TempProject&) = delete;
  ~TempProject() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  void write(const std::string& path, const std::string& text) const {
    const std::filesystem::path file = root_ / path;
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out{file, std::ios::binary | std::ios::trunc};
    out << text;
  }
  void copy_image(const std::string& path, const std::string& fixture) const {
    write(path, read_text(kImages / fixture));
  }
  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

// Remembers the last request the story waited at.
class Seen final : public vnmaki::ScriptObserver {
 public:
  void on_blocked(const BlockingRequest& request, const vnmaki::ScriptPosition& /*at*/) override {
    last = request;
    ++blocked;
  }

  std::optional<BlockingRequest> last;
  int blocked = 0;
};

// A renderer that numbers its textures and shaders and remembers what was destroyed.
class CountingRenderer final : public vnmaki::test::RecordingRenderer {
 public:
  [[nodiscard]] vnmaki::TextureId create_texture(const vnmaki::Image& /*image*/,
                                                 vnmaki::TextureFilter /*filter*/) override {
    return {.value = ++textures};
  }
  [[nodiscard]] vnmaki::ShaderId create_shader(const vnmaki::ShaderBytecode& /*code*/) override {
    return {.value = ++shaders};
  }
  void destroy_texture(vnmaki::TextureId texture) override { destroyed.push_back(texture.value); }

  std::uint32_t textures = 0;
  std::uint32_t shaders = 0;
  std::vector<std::uint32_t> destroyed;
};

// A game in a temp folder, started at its `start` label, with hot reload on top.
struct Game {
  // `setup` writes the other files of the project before the scripts load.
  explicit Game(const std::string& name, const std::string& script,
                const std::function<void(const TempProject&)>& setup = {})
      : project(name),
        vfs(make_vfs(project, script, setup)),
        scene(vfs, kVirtualSize),
        runtime(vfs, options()),
        ui(kVirtualSize),
        hot(runtime, scene, jobs, ui, log) {
    runtime.load_scripts();
    runtime.start("start");
  }

  static vnmaki::Vfs make_vfs(const TempProject& project, const std::string& script,
                              const std::function<void(const TempProject&)>& setup) {
    project.write("scripts/main.lua", script);
    if (setup) {
      setup(project);
    }
    vnmaki::Vfs result;
    REQUIRE(result.mount(vnmaki::make_directory_mount(project.root())));
    return result;
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.observer = &seen;
    result.scene_state = &scene;
    result.scene = &scene;
    result.instruction_limit = kInstructionLimit;
    return result;
  }

  // The script file changes, and its batch arrives.
  void edit(const std::string& script) {
    project.write("scripts/main.lua", script);
    const std::vector<std::string> batch{"scripts/main.lua"};
    hot.apply(batch);
  }

  [[nodiscard]] std::string text() const {
    REQUIRE(seen.last.has_value());
    const auto* narrate = std::get_if<NarrateRequest>(&*seen.last);
    REQUIRE(narrate != nullptr);
    return narrate->text;
  }

  [[nodiscard]] bool logged_error() const {
    for (const vnmaki::LogEntry& entry : log.entries()) {
      if (entry.level == vnmaki::LogLevel::kError) {
        return true;
      }
    }
    return false;
  }

  TempProject project;
  vnmaki::Vfs vfs;
  Seen seen;
  vnmaki::LogBuffer log;
  vnmaki::Scene scene;
  vnmaki::JobSystem jobs{0};
  vnmaki::ScriptRuntime runtime;
  vnmaki::UiHost ui;
  vnmaki::HotReload hot;
};

constexpr std::string_view kThreeLines = R"(
label("start", function()
  narrate("one")
  narrate("two")
  narrate("three")
end)
)";

}  // namespace

TEST_CASE("file watcher: events within the window are one batch, sorted, each path once") {
  vnmaki::FileWatcher watcher;
  const Clock::time_point start = Clock::now();
  watcher.notify("scripts/b.lua", start);
  watcher.notify("scripts/a.lua", start + kQuiet);
  watcher.notify("scripts/b.lua", start + kQuiet);
  CHECK(watcher.poll(start + kQuiet + kQuiet).empty());  // still inside the window
  const std::vector<std::string> batch = watcher.poll(start + kQuiet + vnmaki::kWatchBatchWindow);
  CHECK(batch == std::vector<std::string>{"scripts/a.lua", "scripts/b.lua"});
  CHECK(watcher.poll(start + kQuiet + vnmaki::kWatchBatchWindow * 2).empty());
}

TEST_CASE("file watcher: an event after a batch starts the next batch") {
  vnmaki::FileWatcher watcher;
  const Clock::time_point start = Clock::now();
  watcher.notify("a.lua", start);
  CHECK(watcher.poll(start + vnmaki::kWatchBatchWindow).size() == 1);
  watcher.notify("b.lua", start + vnmaki::kWatchBatchWindow);
  CHECK(watcher.poll(start + vnmaki::kWatchBatchWindow + kQuiet).empty());
  CHECK(watcher.poll(start + vnmaki::kWatchBatchWindow * 2).size() == 1);
}

TEST_CASE("hot reload: a text change shows at the same step, with no new rollback step") {
  Game game{"text", std::string{kThreeLines}};
  game.runtime.advance();  // at "two"
  REQUIRE(game.text() == "two");
  REQUIRE(game.runtime.eval("persistent.seen = 7").ok);
  const vnmaki::StoreSnapshot persistent = game.runtime.persistent_snapshot();
  const std::size_t steps = game.runtime.rollback_steps().size();
  const int blocked = game.seen.blocked;

  game.edit(R"(
label("start", function()
  narrate("one")
  narrate("TWO, changed")
  narrate("three")
end)
)");

  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.runtime.position().label == "start");
  CHECK(game.runtime.position().step == 2);
  CHECK(game.text() == "TWO, changed");
  CHECK(game.seen.blocked == blocked + 1);  // the observer heard the request again
  CHECK(game.runtime.rollback_steps().size() == steps);
  CHECK(game.runtime.persistent_snapshot() == persistent);
  CHECK(game.hot.banner().empty());
  game.runtime.advance();
  CHECK(game.text() == "three");
}

TEST_CASE("hot reload: the story goes on after a reload, and a menu answer is replayed") {
  Game game{"menu", R"(
label("start", function()
  local choice = menu { "left", "right" }
  store.picked = choice
  narrate("after " .. choice)
  narrate("end")
end)
)"};
  game.runtime.choose(1);
  REQUIRE(game.text() == "after 2");

  game.edit(R"(
label("start", function()
  local choice = menu { "left", "right" }
  store.picked = choice
  narrate("then " .. choice)
  narrate("end")
end)
)");

  CHECK(game.text() == "then 2");
  CHECK(game.runtime.position().step == 2);
  game.runtime.advance();
  CHECK(game.text() == "end");
}

TEST_CASE("hot reload: a called label stays on the stack") {
  Game game{"call", R"(
label("start", function()
  call("helper")
  narrate("back")
end)
label("helper", function()
  narrate("in helper")
end)
)"};
  REQUIRE(game.text() == "in helper");

  game.edit(R"(
label("start", function()
  call("helper")
  narrate("back again")
end)
label("helper", function()
  narrate("in the new helper")
end)
)");

  CHECK(game.text() == "in the new helper");
  CHECK(game.runtime.position().label == "helper");
  game.runtime.advance();
  CHECK(game.text() == "back again");
}

TEST_CASE("hot reload: a label that is gone sends the story to start") {
  Game game{"gone", R"(
label("start", function()
  jump("other")
end)
label("other", function()
  narrate("o1")
  narrate("o2")
end)
)"};
  game.runtime.advance();
  REQUIRE(game.runtime.position().label == "other");
  REQUIRE(game.runtime.position().step == 2);

  game.edit(R"(
label("start", function()
  narrate("fresh start")
end)
)");

  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.runtime.position().label == "start");
  CHECK(game.runtime.position().step == 1);
  CHECK(game.text() == "fresh start");
  CHECK(game.hot.banner().empty());
}

TEST_CASE("hot reload: a step past the end of the label stops at the last step") {
  Game game{"short", std::string{kThreeLines}};
  game.runtime.advance();
  game.runtime.advance();
  REQUIRE(game.text() == "three");

  game.edit(R"(
label("start", function()
  narrate("one")
  narrate("two, the last now")
end)
)");

  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.runtime.position().step == 2);
  CHECK(game.text() == "two, the last now");
}

TEST_CASE("hot reload: a syntax error keeps the old scripts, logs the line and shows a banner") {
  Game game{"syntax", std::string{kThreeLines}};
  game.runtime.advance();
  REQUIRE(game.text() == "two");

  game.edit("label(\"start\", function()\n  narrate(\"x\"\nend)\n");

  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.runtime.position().step == 2);
  CHECK(game.text() == "two");
  REQUIRE(game.logged_error());
  CHECK(game.log.entries().back().text.find("scripts/main.lua:3:") != std::string::npos);
  CHECK_FALSE(game.hot.banner().empty());
  CHECK(game.ui.is_shown(vnmaki::kHotReloadScreenName));
  game.runtime.advance();  // the old story still runs
  CHECK(game.text() == "three");

  game.edit(std::string{kThreeLines});  // the file works again
  CHECK(game.hot.banner().empty());
  CHECK_FALSE(game.ui.is_shown(vnmaki::kHotReloadScreenName));
}

TEST_CASE("hot reload: an error while the scripts run keeps the old scripts") {
  Game game{"runtime_error", std::string{kThreeLines}};
  game.edit("error(\"boom\")\n");

  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.text() == "one");
  REQUIRE(game.logged_error());
  CHECK(game.log.entries().back().text.find("scripts/main.lua:1: boom") != std::string::npos);
}

TEST_CASE("hot reload: with no start label the reload fails and the story stays") {
  Game game{"no_start", std::string{kThreeLines}};
  game.edit(R"(
label("elsewhere", function()
  narrate("x")
end)
)");
  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.logged_error());
  CHECK_FALSE(game.hot.banner().empty());
  CHECK(game.text() == "one");
  game.runtime.advance();
  CHECK(game.text() == "two");  // the old scripts still run
}

TEST_CASE("hot reload: the scene is replayed with the new script") {
  Game game{
      "scene",
      R"(
label("start", function()
  show("eileen", { at = "left" })
  narrate("one")
end)
)",
      [](const TempProject& project) { project.copy_image("images/eileen.png", "rgba_2x2.png"); }};
  constexpr std::string_view kWithLucy = R"(
label("start", function()
  show("eileen", { at = "left" })
  show("lucy", { at = "right" })
  narrate("one")
end)
)";
  game.edit(std::string{kWithLucy});
  // There is no image file for "lucy" yet: the replay fails, so the old scripts and the scene stay.
  CHECK(game.logged_error());
  CHECK(game.scene.find("eileen") != nullptr);
  CHECK(game.scene.find("lucy") == nullptr);

  game.project.copy_image("images/lucy.png", "rgba_2x2.png");
  game.edit(std::string{kWithLucy});
  CHECK(game.scene.find("eileen") != nullptr);
  CHECK(game.scene.find("lucy") != nullptr);
  CHECK(game.hot.banner().empty());
}

TEST_CASE("hot reload: a changed image gets a new texture") {
  Game game{
      "image",
      R"(
label("start", function()
  show("eileen")
  narrate("one")
end)
)",
      [](const TempProject& project) { project.copy_image("images/eileen.png", "rgba_2x2.png"); }};
  CountingRenderer renderer;
  game.scene.prepare(renderer, game.jobs);
  REQUIRE(renderer.textures == 1);
  const vnmaki::Displayable* eileen = game.scene.find("eileen");
  REQUIRE(eileen != nullptr);
  CHECK(eileen->bounds().width == doctest::Approx(kImageSmall));

  game.project.copy_image("images/eileen.png", "checker_4x4.png");
  const std::vector<std::string> batch{"images/eileen.png", "images/never_shown.png"};
  game.hot.apply(batch);

  CHECK(renderer.textures == 2);
  CHECK(eileen->bounds().width == doctest::Approx(kImageLarge));
  CHECK(game.hot.banner().empty());
  game.scene.prepare(renderer, game.jobs);  // destroys the old texture
  CHECK(renderer.destroyed == std::vector<std::uint32_t>{1});

  game.project.write("images/eileen.png", "not an image");
  const std::vector<std::string> broken{"images/eileen.png"};
  game.hot.apply(broken);
  CHECK(renderer.textures == 2);  // the old texture stays
  CHECK(eileen->bounds().width == doctest::Approx(kImageLarge));
  CHECK(game.logged_error());
  CHECK_FALSE(game.hot.banner().empty());
}

TEST_CASE("hot reload: a changed shader compiles again, and a compile error keeps the old one") {
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build or on this machine");
    return;
  }
  constexpr std::string_view kGood = R"hlsl(#include "vnmaki.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  return sample_sprite(input) * input.color;
}
)hlsl";
  constexpr std::string_view kBroken = R"hlsl(#include "vnmaki.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  return sample_sprite(input) * missing_name;
}
)hlsl";
  Game game{"shader",
            R"(
shader("plain")
label("start", function()
  show("eileen", { shader = "plain" })
  narrate("one")
end)
)",
            [&](const TempProject& project) {
              project.copy_image("images/eileen.png", "rgba_2x2.png");
              project.write("shaders/plain.frag.hlsl", std::string{kGood});
            }};
  CountingRenderer renderer;
  game.scene.prepare(renderer, game.jobs);
  REQUIRE(renderer.shaders == 1);

  const std::vector<std::string> shader_file{"shaders/plain.frag.hlsl"};
  game.hot.apply(shader_file);  // the same source compiles again
  game.scene.prepare(renderer, game.jobs);
  CHECK(renderer.shaders == 2);
  CHECK(game.hot.banner().empty());

  game.project.write("shaders/plain.frag.hlsl", std::string{kBroken});
  game.hot.apply(shader_file);
  game.scene.prepare(renderer, game.jobs);
  CHECK(renderer.shaders == 2);  // the old shader stays
  REQUIRE(game.logged_error());
  CHECK(game.log.entries().back().text.find("plain.frag.hlsl:4") != std::string::npos);
  CHECK_FALSE(game.hot.banner().empty());

  game.project.write("shaders/plain.frag.hlsl", std::string{kGood});
  game.hot.apply(shader_file);
  CHECK(game.hot.banner().empty());
}

TEST_CASE("hot reload: other files change nothing") {
  Game game{"other", std::string{kThreeLines}};
  const std::vector<std::string> batch{"notes.txt", "scripts/main.lua~", "project.toml", "images"};
  game.hot.apply(batch);
  CHECK(game.log.entries().empty());
  CHECK(game.hot.banner().empty());
  CHECK(game.text() == "one");
  CHECK(game.seen.blocked == 1);
}

TEST_CASE("hot reload: screens come from the new scripts, and a styles file reloads them") {
  Game game{"screens", std::string{kThreeLines}};
  vnmaki::ScreenNodeDef tree;
  CHECK_FALSE(game.runtime.build_screen("extra", {}, tree).empty());  // not defined yet

  game.edit(std::string{kThreeLines} + "\nscreen \"extra\" { text(\"hello\") }\n");
  CHECK(game.runtime.build_screen("extra", {}, tree).empty());

  const int blocked = game.seen.blocked;
  const std::vector<std::string> batch{"styles/dark.lua"};
  game.hot.apply(batch);
  CHECK(game.seen.blocked == blocked + 1);  // the scripts were loaded again
  CHECK(game.runtime.build_screen("extra", {}, tree).empty());
}
