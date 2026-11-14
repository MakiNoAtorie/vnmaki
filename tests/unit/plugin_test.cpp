// The plugin API (task 48, docs/plugins.md): version ranges and manifests, Lua plugins (their
// load order, API functions, transitions, a version mismatch, from a `.vnpak`), and the C++
// plugin context with the example plugin of plugins/example/ (Lua functions, a displayable type
// and its snapshot, an image format). The frame of the displayable is checked on a GPU in
// tests/render/plugin_render_test.cpp; the CLI tests are in tests/cli/CMakeLists.txt.

#include "engine/plugin/plugin.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/anim/transition.hpp"
#include "engine/core/version.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/plugin/lua_plugin.hpp"
#include "engine/render/image.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/check.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/pak_writer.hpp"
#include "engine/vfs/vfs.hpp"
#include "plugins/example/example_plugin.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::PluginContext;
using vnmaki::PluginError;
using vnmaki::PluginParams;
using vnmaki::PluginValue;
using vnmaki::ScriptRuntime;
using vnmaki::VersionRange;

constexpr vnmaki::Size kVirtualSize{.width = 64, .height = 64};
constexpr vnmaki::Version kEngine{.major = 1, .minor = 4, .patch = 2};
constexpr float kBadgeSize = 10;
constexpr float kHalf = 0.5F;
constexpr float kQuarterOfWidth = 16;  // 0.25 of 64
constexpr double kShortFlash = 0.3;    // past the 0.25 s of "flash", short of the 0.5 s of "fade"
constexpr std::size_t kPpmSide = 2;
constexpr std::uint8_t kOpaqueAlpha = 255;

constexpr std::string_view kProject = "title = \"T\"\nplugins = [\"flash\"]\n";
constexpr std::string_view kFlashManifest = "engine = \">=0.1.0\"\n";
constexpr std::string_view kFlashScript =
    "define_transition('flash', 'fade', { color = '#ffffff', duration = 0.25 })\n"
    "function shout(text) return string.upper(text) .. '!' end\n"
    "order = (order or '') .. 'p'\n";

using Files = std::map<std::string, std::string, std::less<>>;

Files flash_game(std::string_view main_script) {
  return {{"project.toml", std::string{kProject}},
          {"plugins/flash/plugin.toml", std::string{kFlashManifest}},
          {"plugins/flash/flash.lua", std::string{kFlashScript}},
          {"scripts/main.lua", std::string{main_script}}};
}

// What load_scripts() throws, or an empty string.
std::string load_error(const vnmaki::Vfs& vfs, const vnmaki::ScriptRuntimeOptions& options = {}) {
  ScriptRuntime runtime{vfs, options};
  try {
    runtime.load_scripts();
  } catch (const vnmaki::ScriptError& error) {
    return error.what();
  }
  return {};
}

const vnmaki::NarrateRequest* narrated(const ScriptRuntime& runtime) {
  return std::get_if<vnmaki::NarrateRequest>(runtime.request());
}

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

// A 2 by 2 binary PPM: red, green / blue, white.
std::string ppm_text() {
  std::string text = "P6\n2 2\n255\n";
  const std::string pixels{"\xff\x00\x00\x00\xff\x00\x00\x00\xff\xff\xff\xff", 12};
  return text + pixels;
}

// Clears the process-wide image formats when a test ends.
struct FormatGuard {
  FormatGuard() = default;
  FormatGuard(const FormatGuard&) = delete;
  FormatGuard& operator=(const FormatGuard&) = delete;
  FormatGuard(FormatGuard&&) = delete;
  FormatGuard& operator=(FormatGuard&&) = delete;
  ~FormatGuard() { vnmaki::clear_image_formats(); }
};

}  // namespace

// --- Version ranges and manifests -------------------------------------------------------------

TEST_CASE("plugin: a version range holds every comparator") {
  const VersionRange range = VersionRange::parse(">=1.2, <2");
  CHECK(range.contains({.major = 1, .minor = 2, .patch = 0}));
  CHECK(range.contains(kEngine));
  CHECK_FALSE(range.contains({.major = 1, .minor = 1, .patch = 9}));
  CHECK_FALSE(range.contains({.major = 2, .minor = 0, .patch = 0}));
  CHECK(VersionRange::parse("=1.4.2").contains(kEngine));
  CHECK(VersionRange::parse("1.4.2").contains(kEngine));
  CHECK_FALSE(VersionRange::parse(">1.4.2").contains(kEngine));
  CHECK(VersionRange::parse("<=1.4.2 >1.4").contains(kEngine));
  CHECK(range.text() == ">=1.2, <2");
}

TEST_CASE("plugin: a bad version range is an error that names the text") {
  for (const char* bad : {"", "  ", ">=", "x", ">=1.x", "1.2.3.4", ">=1., <2", "~1.2"}) {
    INFO(bad);
    CHECK_THROWS_WITH_AS(static_cast<void>(VersionRange::parse(bad)),
                         doctest::Contains("engine version range"), PluginError);
  }
}

TEST_CASE("plugin: a manifest needs an engine range and a matching name") {
  const vnmaki::LuaPluginManifest manifest = vnmaki::parse_plugin_manifest(
      "name = \"flash\"\nversion = \"1.0.0\"\nengine = \">=0.1.0\"\n", "flash");
  CHECK(manifest.name == "flash");
  CHECK(manifest.version == "1.0.0");
  CHECK(manifest.engine.contains(kEngine));

  CHECK_THROWS_WITH_AS(
      static_cast<void>(vnmaki::parse_plugin_manifest("", "flash")),
      doctest::Contains("plugins/flash/plugin.toml: missing required key 'engine'"), PluginError);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(vnmaki::parse_plugin_manifest("name = \"x\"\nengine = \"1\"\n", "flash")),
      doctest::Contains("plugin.toml:1:"), PluginError);
  CHECK_THROWS_WITH_AS(
      static_cast<void>(vnmaki::parse_plugin_manifest("engine = \"nope\"\n", "flash")),
      doctest::Contains("plugin.toml:1:10: engine version range 'nope'"), PluginError);
  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::parse_plugin_manifest("engine = [", "flash")),
                       doctest::Contains("plugin.toml:1:"), PluginError);
}

TEST_CASE("plugin: project.toml lists the plugins") {
  CHECK(vnmaki::parse_project_config("title = \"T\"\n").plugins.empty());
  const vnmaki::ProjectConfig config =
      vnmaki::parse_project_config("title = \"T\"\nplugins = [\"flash\", \"b_2-x\"]\n");
  CHECK(config.plugins == std::vector<std::string>{"flash", "b_2-x"});
  for (const char* bad :
       {"plugins = \"flash\"", "plugins = [1]", "plugins = [\"\"]", "plugins = [\"../x\"]",
        "plugins = [\"A\"]", "plugins = [\"a\", \"a\"]"}) {
    INFO(bad);
    CHECK_THROWS_AS(static_cast<void>(
                        vnmaki::parse_project_config(std::string{"title = \"T\"\n"} + bad + "\n")),
                    vnmaki::ProjectConfigError);
  }
}

// --- Lua plugins ------------------------------------------------------------------------------

TEST_CASE("plugin: a Lua plugin loads before scripts/ and adds API functions") {
  const vnmaki::Vfs vfs =
      vnmaki::test::memory_vfs(flash_game("label('start', function()\n"
                                          "  narrate(shout('hi') .. (order or '?'))\n"
                                          "end)\n"
                                          "order = (order or '') .. 's'\n"));
  ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  REQUIRE(narrated(runtime) != nullptr);
  CHECK(narrated(runtime)->text == "HI!ps");  // the plugin first, then the game's script
}

TEST_CASE("plugin: a plugin's new globals give no warning, a game's do") {
  struct Warnings final : vnmaki::ScriptObserver {
    void on_warning(std::string_view message) override { messages.emplace_back(message); }
    std::vector<std::string> messages;
  } warnings;
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(flash_game("late = 1\n"));
  ScriptRuntime runtime{vfs, {.dev_mode = true, .observer = &warnings}};
  runtime.load_scripts();
  REQUIRE(warnings.messages.size() == 1);
  CHECK(warnings.messages.front().find("new global 'late'") != std::string::npos);
}

TEST_CASE("plugin: a transition defined by a plugin runs its base with its defaults") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      flash_game("label('start', function() transition('flash') end)\n"
                 "label('slow', function() transition('flash', { duration = 1 }) end)\n"
                 "label('plain', function() transition('fade') end)\n"
                 "label('bad', function() transition('fade', { duration = 'x' }) end)\n"));
  const auto elapsed = [&](const char* label) {
    vnmaki::Scene scene{vfs, kVirtualSize};
    ScriptRuntime runtime{vfs, {.scene_state = &scene, .scene = &scene}};
    runtime.load_scripts();
    runtime.start(label);
    REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
    scene.update(kShortFlash);
    return scene.tweening(vnmaki::kTransitionTag);
  };
  CHECK_FALSE(elapsed("start"));  // 0.25 s from the plugin
  CHECK(elapsed("slow"));         // the script's duration beats the default
  CHECK(elapsed("plain"));        // "fade" itself is 0.5 s
}

TEST_CASE("plugin: a name of define_transition cannot stand on another alias") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua",
        "define_transition('a', 'fade', {})\ndefine_transition('b', 'a', {})"}});
  CHECK(load_error(vfs).find("itself defined by define_transition") != std::string::npos);
}

TEST_CASE("plugin: an engine outside the range stops the start with a clear error") {
  Files files = flash_game("label('start', function() narrate('x') end)\n");
  files["plugins/flash/plugin.toml"] = "engine = \">=9.0.0\"\n";
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(std::move(files));
  const std::string error = load_error(vfs);
  CHECK(error.find("plugin 'flash' needs vnmaki >=9.0.0, but this is vnmaki ") !=
        std::string::npos);
  CHECK(error.find("(plugins/flash/plugin.toml)") != std::string::npos);
}

TEST_CASE("plugin: a plugin that is listed but missing, and a script error in one") {
  Files files = flash_game("");
  files.erase("plugins/flash/plugin.toml");
  CHECK(load_error(vnmaki::test::memory_vfs(files)) ==
        "plugin 'flash' is in project.toml, but plugins/flash/plugin.toml is missing");

  files = flash_game("");
  files["plugins/flash/flash.lua"] = "local x = 1\nerror('broken')\n";
  const std::string error = load_error(vnmaki::test::memory_vfs(std::move(files)));
  CHECK(error.find("plugins/flash/flash.lua:2: broken") != std::string::npos);
}

TEST_CASE("plugin: a Lua plugin works from a .vnpak") {
  std::ostringstream stream;
  {
    vnmaki::PakWriter pak{stream};
    for (const auto& [path, text] :
         flash_game("label('start', function() narrate(shout('go')) end)")) {
      pak.add(path, bytes_of(text));
    }
    pak.finish();
  }
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::PakMount::from_bytes(bytes_of(stream.str()))));
  ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  REQUIRE(narrated(runtime) != nullptr);
  CHECK(narrated(runtime)->text == "GO!");
}

TEST_CASE("plugin: vnmaki check does not flag what a plugin defines") {
  const vnmaki::Vfs vfs =
      vnmaki::test::memory_vfs(flash_game("label('start', function()\n"
                                          "  transition('flash')\n"
                                          "  show('badge', { displayable = 'example_badge' })\n"
                                          "  local line = shout('hi')\n"
                                          "  narrate('done')\n"
                                          "end, { ending = true })\n"));
  CHECK(vnmaki::findings_to_csv(vnmaki::check_project(vfs)) == "file,line,severity,code,message\n");
}

// --- C++ plugins ------------------------------------------------------------------------------

TEST_CASE("plugin: the context refuses a name that is taken or not a Lua name") {
  PluginContext context;
  const auto zero = [](std::span<const PluginValue>) -> PluginValue { return std::monostate{}; };
  context.add_function("one", zero);
  CHECK_THROWS_AS(context.add_function("one", zero), PluginError);
  CHECK_THROWS_AS(context.add_function("end", zero), PluginError);
  CHECK_THROWS_AS(context.add_function("a b", zero), PluginError);
  CHECK_THROWS_AS(context.add_function("1a", zero), PluginError);
  CHECK_THROWS_AS(context.add_function("two", nullptr), PluginError);
  CHECK_THROWS_AS(context.add_displayable("", {}), PluginError);
  CHECK_THROWS_AS(context.add_image_loader(".x", nullptr), PluginError);
  const FormatGuard formats;
  context.add_image_loader(".x", [](std::span<const std::byte>) { return vnmaki::PluginImage{}; });
  CHECK_THROWS_AS(context.add_image_loader(
                      ".x", [](std::span<const std::byte>) { return vnmaki::PluginImage{}; }),
                  PluginError);
  CHECK_THROWS_AS(context.add_image_loader(
                      ".png", [](std::span<const std::byte>) { return vnmaki::PluginImage{}; }),
                  PluginError);
}

TEST_CASE("plugin: Lua calls a function of a C++ plugin") {
  const FormatGuard formats;
  PluginContext context;
  vnmaki::register_plugin_example(context);
  const vnmaki::Vfs vfs =
      vnmaki::test::memory_vfs({{"scripts/main.lua",
                                 "label('start', function()\n"
                                 "  narrate(example_greet('Maki') .. ' ' .. example_add(2, 3) .. ' "
                                 "' .. example_add(1.5, 2))\n"
                                 "end)\n"
                                 "label('bad', function()\n"
                                 "  narrate(example_add('a', 1))\n"
                                 "end)\n"
                                 "label('table', function()\n"
                                 "  narrate(example_greet({}))\n"
                                 "end)\n"}});
  ScriptRuntime runtime{vfs, {.plugins = &context}};
  runtime.load_scripts();
  runtime.start("start");
  REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
  REQUIRE(narrated(runtime) != nullptr);
  CHECK(narrated(runtime)->text == "Hello, Maki! 5 3.5");

  ScriptRuntime failing{vfs, {.plugins = &context}};
  failing.load_scripts();
  failing.start("bad");
  CHECK(failing.state() == vnmaki::RunState::kFailed);
  CHECK(failing.error().find("scripts/main.lua:5: example_add: needs two numbers") !=
        std::string::npos);

  ScriptRuntime nested{vfs, {.plugins = &context}};
  nested.load_scripts();
  nested.start("table");
  CHECK(nested.state() == vnmaki::RunState::kFailed);
  CHECK(nested.error().find("only nil, booleans, numbers and strings") != std::string::npos);
}

TEST_CASE("plugin: a function may not replace a global of the engine") {
  PluginContext context;
  context.add_function(
      "say", [](std::span<const PluginValue>) -> PluginValue { return std::monostate{}; });
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", ""}});
  CHECK(load_error(vfs, {.plugins = &context}) ==
        "plugin function 'say' would replace a global of the engine");
}

TEST_CASE("plugin: a displayable of a plugin is shown, tweened, kept in a snapshot and hidden") {
  const FormatGuard formats;
  PluginContext context;
  vnmaki::register_plugin_example(context);
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua",
        "label('start', function()\n"
        "  show('a', { displayable = 'example_badge', params = { size = 10, color = '#336699' },\n"
        "              x = 0.25, y = 0.5 })\n"
        "  show('b', { displayable = 'example_badge', layer = 'overlay' })\n"
        "  show('c', { displayable = 'example_badge' })\n"
        "  tween('a', { alpha = 0.5, duration = 1 })\n"
        "  narrate('shown')\n"
        "end)\n"
        "label('unknown', function() show('a', { displayable = 'nothing' }) end)\n"
        "label('size', function()\n"
        "  show('a', { displayable = 'example_badge', params = { size = 0 } })\n"
        "end)\n"
        "label('color', function()\n"
        "  show('a', { displayable = 'example_badge', params = { color = 5 } })\n"
        "end)\n"
        "label('table', function()\n"
        "  show('a', { displayable = 'example_badge', params = { size = {} } })\n"
        "end)\n"}});
  const auto play = [&](vnmaki::Scene& scene, const char* label) {
    auto runtime = std::make_unique<ScriptRuntime>(
        vfs,
        vnmaki::ScriptRuntimeOptions{.scene_state = &scene, .scene = &scene, .plugins = &context});
    runtime->load_scripts();
    runtime->start(label);
    return runtime;
  };

  vnmaki::Scene scene{vfs, kVirtualSize};
  scene.set_plugins(&context);
  const std::unique_ptr<ScriptRuntime> runtime = play(scene, "start");
  REQUIRE_MESSAGE(runtime->state() == vnmaki::RunState::kWaiting, runtime->error());
  const auto* badge = dynamic_cast<const vnmaki::PluginDisplayable*>(scene.find("a"));
  REQUIRE(badge != nullptr);
  CHECK(badge->type() == "example_badge");
  CHECK(vnmaki::number_param(badge->params(), "size", 0) == kBadgeSize);
  CHECK(vnmaki::string_param(badge->params(), "color", "") == "#336699");
  CHECK(badge->properties().position.x == kQuarterOfWidth);
  CHECK(scene.layer_of("b") == vnmaki::Layer::kOverlay);
  CHECK(badge->bounds().width == kBadgeSize);
  CHECK(scene.tags(vnmaki::Layer::kCharacters) == std::vector<std::string>{"a", "c"});

  // A tween reaches it like any displayable, and a snapshot keeps its type, params and place.
  scene.update(1.0);
  CHECK(scene.find("a")->properties().alpha == kHalf);
  const std::vector<std::byte> snapshot = scene.capture();
  vnmaki::Scene restored{vfs, kVirtualSize};
  restored.set_plugins(&context);
  restored.restore(snapshot);
  const auto* again = dynamic_cast<const vnmaki::PluginDisplayable*>(restored.find("a"));
  REQUIRE(again != nullptr);
  CHECK(again->params() == badge->params());
  CHECK(again->properties() == badge->properties());
  CHECK(restored.tags(vnmaki::Layer::kCharacters) == std::vector<std::string>{"a", "c"});
  CHECK(restored.tags(vnmaki::Layer::kOverlay) == std::vector<std::string>{"b"});
  CHECK(restored.capture() == snapshot);

  // A game without the plugin cannot load the snapshot, and the scene stays as it was.
  const PluginContext none;
  vnmaki::Scene without{vfs, kVirtualSize};
  without.set_plugins(&none);
  CHECK_THROWS_WITH_AS(without.restore(snapshot), doctest::Contains("is not in any plugin"),
                       std::invalid_argument);
  CHECK(without.tags(vnmaki::Layer::kCharacters).empty());

  scene.hide("a");
  CHECK(scene.find("a") == nullptr);

  // Errors reach the script at the line of the show.
  const auto failure = [&](const char* label) {
    vnmaki::Scene bad{vfs, kVirtualSize};
    bad.set_plugins(&context);
    const std::unique_ptr<ScriptRuntime> failing = play(bad, label);
    CHECK(failing->state() == vnmaki::RunState::kFailed);
    return failing->error();
  };
  CHECK(failure("unknown").find("scripts/main.lua:9: unknown displayable type 'nothing' (the "
                                "types are example_badge)") != std::string::npos);
  CHECK(failure("size").find("displayable type 'example_badge': param 'size' must be from 1 to "
                             "4096") != std::string::npos);
  CHECK(failure("color").find("param 'color' must be a string") != std::string::npos);
  CHECK(failure("table").find("only nil, booleans, numbers and strings") != std::string::npos);
}

TEST_CASE("plugin: without a plugin, a displayable type is unknown") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua", "label('start', function() show('a', { displayable = 'x' }) end)"}});
  vnmaki::Scene scene{vfs, kVirtualSize};
  const PluginContext none;
  scene.set_plugins(&none);
  ScriptRuntime runtime{vfs, {.scene_state = &scene, .scene = &scene}};
  runtime.load_scripts();
  runtime.start("start");
  CHECK(runtime.error().find("unknown displayable type 'x' (no plugin adds one)") !=
        std::string::npos);
}

TEST_CASE("plugin: an image format of a plugin is found, decoded and checked") {
  const FormatGuard formats;
  PluginContext context;
  vnmaki::register_plugin_example(context);
  CHECK(vnmaki::image_format_extensions() == std::vector<std::string>{".ppm"});
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"images/pic.ppm", ppm_text()},
                                                    {"images/short.ppm", "P6\n2 2\n255\nxx"},
                                                    {"images/both.png", "x"},
                                                    {"images/both.ppm", ppm_text()}});
  CHECK(vnmaki::resolve_image(vfs, "pic") == "images/pic.ppm");
  CHECK(vnmaki::resolve_image(vfs, "both") == "images/both.png");  // built-in formats first
  CHECK_FALSE(vnmaki::resolve_image(vfs, "none").has_value());

  vnmaki::JobSystem jobs;
  const vnmaki::Image image = vnmaki::load_image(vfs, jobs, "images/pic.ppm").get();
  REQUIRE(image.size == vnmaki::Size{.width = kPpmSide, .height = kPpmSide});
  const std::vector<std::uint8_t> expected{255, 0, 0,   kOpaqueAlpha, 0,   255, 0,   kOpaqueAlpha,
                                           0,   0, 255, kOpaqueAlpha, 255, 255, 255, kOpaqueAlpha};
  CHECK(image.pixels == expected);
  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::load_image(vfs, jobs, "images/short.ppm").get()),
                       doctest::Contains("cannot decode image 'images/short.ppm': PPM"),
                       vnmaki::ImageError);

  vnmaki::clear_image_formats();
  CHECK_FALSE(vnmaki::resolve_image(vfs, "pic").has_value());
}
