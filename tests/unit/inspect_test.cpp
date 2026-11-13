// The row functions of the dev inspectors (engine/tools/inspect.hpp), on the default template
// (templates/default) played to its first line, and on small scripts for the odd cases.

#include "engine/tools/inspect.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::StoreRow;

constexpr vnmaki::Size kCanvas{.width = 1280, .height = 720};
constexpr double kHalfSecond = 0.5;
constexpr double kTweenSeconds = 2;
constexpr double kTweenTarget = 0.8;
constexpr double kEditX = 321;
constexpr double kTooBig = 5;
constexpr std::size_t kLayerCount = 7;
constexpr int kChangedTextSpeed = 60;

// The template, played to its first line (the title screen is answered, the dissolve is over).
class TemplateGame {
 public:
  TemplateGame()
      : scene_(vfs_, kCanvas),
        audio_(vfs_),
        state_({&scene_, &audio_}),
        runtime_(vfs_, {.scene_state = &state_, .scene = &scene_, .audio = &audio_}) {
    REQUIRE(vfs_.mount(vnmaki::make_directory_mount(VNMAKI_TEMPLATES_DIR "/default")));
    runtime_.load_scripts();
    runtime_.start("start");
    runtime_.return_screen(vnmaki::ScreenValue{std::string{"start"}});
    settle();
  }

  // Ends the waits, as the headless runner does.
  void settle() {
    while (const auto* wait = std::get_if<vnmaki::WaitRequest>(runtime_.request())) {
      scene_.finish_tweens(wait->tag);
      runtime_.advance();
    }
  }

  [[nodiscard]] vnmaki::ScriptRuntime& runtime() { return runtime_; }
  [[nodiscard]] vnmaki::Scene& scene() { return scene_; }
  [[nodiscard]] vnmaki::Audio& audio() { return audio_; }

 private:
  vnmaki::Vfs vfs_;
  vnmaki::Scene scene_;
  vnmaki::Audio audio_;
  vnmaki::CompositeSceneState state_;
  vnmaki::ScriptRuntime runtime_;
};

const StoreRow* find_row(const std::vector<StoreRow>& rows, std::string_view key) {
  const auto found = std::ranges::find(rows, key, &StoreRow::key);
  return found == rows.end() ? nullptr : &*found;
}

double property(const vnmaki::Scene& scene, std::string_view tag, std::string_view name) {
  for (const vnmaki::PropertyRow& row : vnmaki::inspect_properties(scene, tag)) {
    if (row.name == name) {
      return row.value;
    }
  }
  FAIL("no property " << name);
  return 0;
}

}  // namespace

TEST_CASE("inspect: store is a tree of its fields") {
  TemplateGame game;
  const std::vector<StoreRow> rows = vnmaki::inspect_store(game.runtime());
  const StoreRow* affection = find_row(rows, "affection");
  REQUIRE(affection != nullptr);
  CHECK(affection->depth == 0);
  CHECK(affection->value == "0");
  CHECK_FALSE(affection->is_table);

  REQUIRE(game.runtime().eval("store.bag = { gold = 3, tags = { 'a', 'b' }, name = 'x' }").ok);
  const std::vector<StoreRow> nested = vnmaki::inspect_store(game.runtime());
  const StoreRow* bag = find_row(nested, "bag");
  REQUIRE(bag != nullptr);
  CHECK(bag->is_table);
  CHECK(bag->value.empty());
  const StoreRow* gold = find_row(nested, "gold");
  REQUIRE(gold != nullptr);
  CHECK(gold->depth == bag->depth + 1);
  CHECK(gold->value == "3");
  CHECK(find_row(nested, "name")->value == "\"x\"");  // strings are quoted
  CHECK(find_row(nested, "tags")->depth == bag->depth + 1);
  CHECK(std::ranges::find(nested, "1", &StoreRow::key)->depth == bag->depth + 2);
  CHECK(gold > bag);  // a table's fields follow its row
}

TEST_CASE("inspect: a table that two fields share is expanded once") {
  TemplateGame game;
  REQUIRE(game.runtime().eval("local t = { n = 1 }; store.a = t; store.b = t").ok);
  const std::vector<StoreRow> rows = vnmaki::inspect_store(game.runtime());
  const auto expanded = std::ranges::count_if(rows, [](const StoreRow& row) {
    return row.key == "n";
  });
  CHECK(expanded == 1);
  const StoreRow* a = find_row(rows, "a");
  const StoreRow* b = find_row(rows, "b");
  REQUIRE((a != nullptr && b != nullptr));
  CHECK(a->value.empty() != b->value.empty());  // one expanded, one says it is the same table
}

TEST_CASE("inspect: persistent shows what the player changed") {
  TemplateGame game;
  REQUIRE(game.runtime().eval("prefs.text_speed = " + std::to_string(kChangedTextSpeed)).ok);
  const std::vector<StoreRow> rows = vnmaki::inspect_persistent(game.runtime());
  const StoreRow* prefs = find_row(rows, "prefs");
  REQUIRE(prefs != nullptr);
  CHECK(prefs->is_table);
  const StoreRow* speed = find_row(rows, "text_speed");
  REQUIRE(speed != nullptr);
  CHECK(speed->depth == prefs->depth + 1);
  CHECK(speed->value == std::to_string(kChangedTextSpeed));
}

TEST_CASE("inspect: the scene graph lists every layer and its displayables") {
  TemplateGame game;
  const std::vector<vnmaki::LayerRows> layers = vnmaki::inspect_scene(game.scene());
  REQUIRE(layers.size() == kLayerCount);
  CHECK(layers.front().layer == "background");
  CHECK(layers.back().layer == "transient");

  REQUIRE(layers.front().displayables.size() == 1);
  CHECK(layers.front().displayables.front().tag == "background");
  CHECK(layers.front().displayables.front().kind.starts_with("image "));
  CHECK(layers.front().displayables.front().kind.find("classroom") != std::string::npos);

  const auto characters = std::ranges::find(layers, "characters", &vnmaki::LayerRows::layer);
  REQUIRE(characters != layers.end());
  REQUIRE(characters->displayables.size() == 1);
  CHECK(characters->displayables.front().tag == "eileen");
  CHECK(characters->displayables.front().kind == "layered");
  CHECK(layers.at(1).displayables.empty());  // master
}

TEST_CASE("inspect: displayable properties, and an edit is live but not in the snapshot") {
  TemplateGame game;
  vnmaki::Scene& scene = game.scene();
  CHECK(vnmaki::inspect_properties(scene, "nobody").empty());
  CHECK(property(scene, "eileen", "alpha") == doctest::Approx(1));
  CHECK(property(scene, "eileen", "scale_x") == doctest::Approx(1));

  const std::vector<std::byte> snapshot_scene = game.runtime().label_snapshot()->scene;
  const std::vector<std::byte> live_before = scene.capture();
  CHECK(vnmaki::set_property(scene, "eileen", "x", kEditX).empty());
  CHECK(property(scene, "eileen", "x") == doctest::Approx(kEditX));
  CHECK(scene.find("eileen")->properties().position.x == doctest::Approx(kEditX));
  CHECK(game.runtime().label_snapshot()->scene == snapshot_scene);  // the snapshot is as it was
  CHECK(scene.capture() != live_before);                            // the live scene changed

  CHECK(vnmaki::set_property(scene, "eileen", "alpha", kTooBig).empty());
  CHECK(property(scene, "eileen", "alpha") == doctest::Approx(1));  // kept in [0, 1]
  CHECK(vnmaki::set_property(scene, "eileen", "bogus", 1).find("no property") !=
        std::string::npos);
  CHECK_FALSE(vnmaki::set_property(scene, "nobody", "x", 1).empty());
  CHECK_FALSE(vnmaki::set_property(scene, "eileen", "x", std::numeric_limits<double>::infinity()).empty());
}

TEST_CASE("inspect: a rollback brings back a property that was edited") {
  TemplateGame game;
  vnmaki::Scene& scene = game.scene();
  game.runtime().advance();  // the menu
  game.runtime().choose(0);  // the warm branch: say 1
  game.settle();
  const double before = property(scene, "eileen", "x");
  CHECK(vnmaki::set_property(scene, "eileen", "x", kEditX).empty());
  REQUIRE(game.runtime().roll_back());
  CHECK(property(scene, "eileen", "x") == doctest::Approx(before));
}

TEST_CASE("inspect: running tweens, with their properties and time") {
  TemplateGame game;
  CHECK(vnmaki::inspect_tweens(game.scene()).empty());
  const vnmaki::TweenRequest request{
      .tag = "eileen", .x = kTweenTarget, .alpha = kHalfSecond, .duration = kTweenSeconds};
  REQUIRE(game.scene().tween(request).empty());

  std::vector<vnmaki::TweenRow> rows = vnmaki::inspect_tweens(game.scene());
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().tag == "eileen");
  CHECK(rows.front().channels == "x, alpha");
  CHECK(rows.front().ease == "linear");
  CHECK(rows.front().duration == doctest::Approx(kTweenSeconds));
  CHECK(rows.front().elapsed == doctest::Approx(0));
  CHECK_FALSE(rows.front().loop);

  game.scene().update(1);
  rows = vnmaki::inspect_tweens(game.scene());
  REQUIRE(rows.size() == 1);
  CHECK(rows.front().elapsed == doctest::Approx(1));

  game.scene().update(kTweenSeconds);  // it ends
  CHECK(vnmaki::inspect_tweens(game.scene()).empty());
}

TEST_CASE("inspect: audio channels with file, volume and position") {
  TemplateGame game;
  game.audio().update(kHalfSecond);
  const std::vector<vnmaki::AudioRow> rows = vnmaki::inspect_audio(game.audio());
  CHECK(rows.size() >= 2);  // the music, and the channels that are silent
  const auto music = std::ranges::find(rows, "music", &vnmaki::AudioRow::channel);
  REQUIRE(music != rows.end());
  CHECK(music->file.find("theme") != std::string::npos);
  CHECK(music->loop);
  CHECK(music->volume == doctest::Approx(1));
  CHECK(music->position > 0);
  const auto sound = std::ranges::find(rows, "sound", &vnmaki::AudioRow::channel);
  REQUIRE(sound != rows.end());
  CHECK(sound->file.empty());
}

TEST_CASE("inspect: labels, and Jump starts a label at step 1") {
  TemplateGame game;
  vnmaki::ScriptRuntime& runtime = game.runtime();
  const std::vector<vnmaki::LabelRow> labels = vnmaki::inspect_labels(runtime);
  std::vector<std::string> names;
  for (const vnmaki::LabelRow& label : labels) {
    names.push_back(label.name);
    CHECK(label.current == (label.name == "classroom"));
  }
  CHECK(names == std::vector<std::string>{"classroom", "cold", "start", "warm"});

  REQUIRE(runtime.store_snapshot().tables.size() >= 1);
  CHECK(vnmaki::jump_to_label(runtime, "cold").empty());
  CHECK(runtime.state() == vnmaki::RunState::kWaiting);
  CHECK(runtime.position() == vnmaki::ScriptPosition{.label = "cold", .step = 1});
  const auto* say = std::get_if<vnmaki::SayRequest>(runtime.request());
  REQUIRE(say != nullptr);
  CHECK(say->text == "Oh. So that is the only reason.");
  CHECK(vnmaki::inspect_labels(runtime).at(1).current);

  runtime.advance();  // the story goes on from there
  CHECK(runtime.position().step == 2);

  CHECK(vnmaki::jump_to_label(runtime, "nowhere").find("no label") != std::string::npos);
  CHECK(runtime.position().label == "cold");
}

TEST_CASE("inspect: Jump to a label that cannot reach step 1 leaves the story where it was") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua",
                                                     "label('start', function()\n"
                                                     "  narrate('one')\n"
                                                     "  narrate('two')\n"
                                                     "end)\n"
                                                     "label('quiet', function() end)\n"}});
  vnmaki::ScriptRuntime runtime{vfs};
  runtime.load_scripts();
  runtime.start("start");
  runtime.advance();
  REQUIRE(runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 2});

  const std::string problem = vnmaki::jump_to_label(runtime, "quiet");
  CHECK_FALSE(problem.empty());
  CHECK(runtime.state() == vnmaki::RunState::kWaiting);
  CHECK(runtime.position() == vnmaki::ScriptPosition{.label = "start", .step = 2});
}
