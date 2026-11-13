// `vnmaki new` and the default template (docs/SPEC.md section 19): the copy, the title, the
// template lookup, and the sample game played from `new` to the end of both branches, with a save,
// a load and a rollback. The presentation demo (label `presentation`, task 28) is played the same
// way: a save during a tween, a load, and a rollback over a transition. Expected lines are written
// out by hand from templates/default/scripts.

#include "engine/runtime/new_project.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/sdl_platform.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/saves.hpp"
#include "engine/scene/layered_sprite.hpp"
#include "engine/scene/scene.hpp"
#include "engine/text/text.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using vnmaki::NarrateRequest;
using vnmaki::RunState;
using vnmaki::SayRequest;

constexpr std::uint32_t kEileenColor = 0xC8A2C8;
constexpr std::size_t kSampleMenuOptions = 4;
constexpr std::size_t kPresentationOption = 2;  // 0-based: the third option of the menu
constexpr std::string_view kFontPath = "fonts/default.ttf";
constexpr float kFontSize = 40;
constexpr float kNoWrap = 100000;
// Thirteen images, the music, the chime, the voice line, two fonts, the shader and the 3D arm.
constexpr std::size_t kAssetCount = 20;

// A directory under the system temp directory, empty at the start and removed at the end.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_new_test_", name)) {
    fs::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    std::error_code ignored;  // a directory left in the temp directory is not worth a failure
    fs::remove_all(path_, ignored);
  }

  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

std::string read_file(const fs::path& path) {
  std::ifstream stream{path, std::ios::binary};
  return std::string{std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

void write_file(const fs::path& path, std::string_view content) {
  fs::create_directories(path.parent_path());
  std::ofstream stream{path, std::ios::binary};
  stream << content;
}

std::string error_of(const fs::path& template_dir, const fs::path& target_dir,
                     std::string_view title) {
  try {
    vnmaki::create_project(template_dir, target_dir, title);
  } catch (const vnmaki::NewProjectError& error) {
    return error.what();
  }
  return {};
}

const fs::path& default_template() {
  static const fs::path path = fs::path{VNMAKI_TEMPLATES_DIR} / "default";
  return path;
}

}  // namespace

TEST_CASE("new: the title is the last part of the name") {
  CHECK(vnmaki::project_title("games/my_game") == "my_game");
  CHECK(vnmaki::project_title("games/my_game/") == "my_game");
  CHECK(vnmaki::project_title("games/other/..") == "games");
  CHECK_FALSE(vnmaki::project_title(".").empty());  // the name of the current folder
  CHECK_THROWS_AS(static_cast<void>(vnmaki::project_title("tab\there")), vnmaki::NewProjectError);
}

TEST_CASE("new: copies the files and puts the title into toml and lua files only") {
  const TempDir temp{"copy"};
  const fs::path source = temp.path() / "template";
  std::string binary = "\x89PNG {{title}}\r\n";
  binary += '\0';
  binary += '\xFF';
  write_file(source / "project.toml", "title = \"{{title}}\"\n");
  write_file(source / "scripts/main.lua", "print(\"{{title}}\", \"{{title}}\")\n");
  write_file(source / "images/a.png", binary);
  write_file(source / "notes.txt", "{{title}}");

  vnmaki::create_project(source, temp.path() / "game", "Say \"hi\" \\ now");

  CHECK(read_file(temp.path() / "game/project.toml") == "title = \"Say \\\"hi\\\" \\\\ now\"\n");
  CHECK(read_file(temp.path() / "game/scripts/main.lua") ==
        "print(\"Say \\\"hi\\\" \\\\ now\", \"Say \\\"hi\\\" \\\\ now\")\n");
  CHECK(read_file(temp.path() / "game/images/a.png") == binary);
  CHECK(read_file(temp.path() / "game/notes.txt") == "{{title}}");
}

TEST_CASE("new: stops if the folder is not empty, and fills an empty one") {
  const TempDir temp{"target"};
  const fs::path source = temp.path() / "template";
  write_file(source / "project.toml", "title = \"{{title}}\"\n");

  write_file(temp.path() / "busy/keep.txt", "mine");
  CHECK(error_of(source, temp.path() / "busy", "T").ends_with("is not an empty folder"));
  CHECK(read_file(temp.path() / "busy/keep.txt") == "mine");
  CHECK_FALSE(fs::exists(temp.path() / "busy/project.toml"));

  write_file(temp.path() / "a_file", "x");
  CHECK(error_of(source, temp.path() / "a_file", "T").ends_with("is not an empty folder"));

  fs::create_directories(temp.path() / "empty");
  CHECK(error_of(source, temp.path() / "empty", "T").empty());
  CHECK(fs::exists(temp.path() / "empty/project.toml"));

  CHECK(error_of(source, temp.path() / "again", "T").empty());  // a folder that does not exist
  CHECK(error_of(source, temp.path() / "again", "T").ends_with("is not an empty folder"));
  CHECK(error_of(source, temp.path() / "title", "bad\ntitle").find("control character") !=
        std::string::npos);
  CHECK_FALSE(fs::exists(temp.path() / "title"));
}

TEST_CASE("new: finds the template next to the executable first, then in the source tree") {
  const TempDir temp{"find"};
  write_file(temp.path() / "bin/templates/default/project.toml", "");
  write_file(temp.path() / "source/default/project.toml", "");
  write_file(temp.path() / "source/other/project.toml", "");
  write_file(temp.path() / "source/broken/readme.txt", "");

  CHECK(vnmaki::find_template("default", temp.path() / "bin", temp.path() / "source") ==
        temp.path() / "bin/templates/default");
  CHECK(vnmaki::find_template("other", temp.path() / "bin", temp.path() / "source") ==
        temp.path() / "source/other");
  CHECK_FALSE(vnmaki::find_template("broken", temp.path() / "bin", temp.path() / "source"));
  CHECK_FALSE(vnmaki::find_template("none", temp.path() / "bin", temp.path() / "source"));
  CHECK(vnmaki::find_template("default", vnmaki::executable_directory(), VNMAKI_TEMPLATES_DIR));
}

namespace {

// The sample game, made by `new` into a folder of its own and run like `vnmaki run` does it, with
// the saves in another temporary folder (what --data-dir gives).
class SampleGame {
 public:
  explicit SampleGame(const std::string& name)
      : temp_(name),
        project_(temp_.path() / "my_game"),
        config_(prepare()),
        scene_(vfs_, {.width = config_.resolution.width, .height = config_.resolution.height}),
        audio_(vfs_),
        state_({&scene_, &audio_}),
        runtime_(vfs_, {.rollback_depth = config_.rollback_depth,
                        .scene_state = &state_,
                        .scene = &scene_,
                        .audio = &audio_}),
        saves_(runtime_, {.directory = temp_.path() / "data",
                          .game_version = config_.version,
                          .auto_save = config_.auto_save}) {
    scene_.set_voice_level(&audio_);  // lip sync, as `vnmaki run` does it
    runtime_.load_scripts();
    runtime_.start("start");
    settle();
  }

  [[nodiscard]] vnmaki::ScriptRuntime& runtime() { return runtime_; }
  [[nodiscard]] vnmaki::Saves& saves() { return saves_; }
  [[nodiscard]] const vnmaki::ProjectConfig& config() const { return config_; }
  [[nodiscard]] vnmaki::Scene& scene() { return scene_; }

  // Moves the fake clock of the scene and of the audio, which has no device here.
  void step(double seconds) {
    audio_.update(seconds);
    scene_.update(seconds);
  }

  // Ends the waits (the dissolve of the first scene) as the headless runner does, and passes a
  // movie, which is not played here. A tween that a script started and does not wait for goes on.
  // The host polls the saves whenever the story waits, so the auto save sees every position.
  void settle() {
    saves_.poll();
    while (const vnmaki::BlockingRequest* request = runtime_.request()) {
      if (const auto* wait = std::get_if<vnmaki::WaitRequest>(request)) {
        scene_.finish_tweens(wait->tag);
      } else if (!std::holds_alternative<vnmaki::MovieRequest>(*request)) {
        return;
      }
      runtime_.advance();
      saves_.poll();
    }
  }
  void advance() {
    runtime_.advance();
    settle();
  }
  void choose(std::size_t index) {
    runtime_.choose(index);
    settle();
  }
  [[nodiscard]] bool roll_back() {
    const bool went_back = runtime_.roll_back();
    settle();
    return went_back;
  }
  [[nodiscard]] std::string load(std::string_view slot) {
    std::string error = saves_.load(slot);
    settle();
    return error;
  }

  // "Eileen: You came back.", "narrate: ...", "menu", "screen title" or "ended".
  [[nodiscard]] std::string at() const {
    const vnmaki::BlockingRequest* request = runtime_.request();
    if (request == nullptr) {
      return runtime_.state() == RunState::kFinished ? "ended" : "no request";
    }
    if (const auto* say = std::get_if<SayRequest>(request)) {
      return say->speaker + ": " + say->text;
    }
    if (const auto* narrate = std::get_if<NarrateRequest>(request)) {
      return "narrate: " + narrate->text;
    }
    if (std::holds_alternative<vnmaki::MenuRequest>(*request)) {
      return "menu";
    }
    if (const auto* screen = std::get_if<vnmaki::CallScreenRequest>(request)) {
      return "screen " + screen->name;
    }
    return "other request";
  }

  // The title screen, then the first line.
  void start_story() {
    REQUIRE(at() == "screen title");
    runtime_.return_screen(vnmaki::ScreenValue{std::string{"start"}});
    settle();
  }

 private:
  // `vnmaki new`, then mounts the folder as `vnmaki run` does.
  vnmaki::ProjectConfig prepare() {
    vnmaki::create_project(default_template(), project_, vnmaki::project_title(project_));
    REQUIRE(vfs_.mount(vnmaki::make_directory_mount(project_)));
    if (!vnmaki::shader_compiler_available()) {  // a release build loads the precompiled sepia
      REQUIRE(vfs_.mount(vnmaki::make_directory_mount(VNMAKI_PRECOMPILED_DIR)));
    }
    return vnmaki::load_project_config(vfs_);
  }

  TempDir temp_;
  fs::path project_;
  vnmaki::Vfs vfs_;
  vnmaki::ProjectConfig config_;
  vnmaki::Scene scene_;
  vnmaki::Audio audio_;
  vnmaki::CompositeSceneState state_;
  vnmaki::ScriptRuntime runtime_;
  vnmaki::Saves saves_;
};

constexpr std::string_view kFirstLine = "Eileen: You came back.";
constexpr std::string_view kWarmLine = "Eileen: I'm glad. I kept your seat by the window.";
constexpr std::string_view kWarmLast = "Eileen: Let's walk home together.";
constexpr std::string_view kColdLine = "Eileen: Oh. So that is the only reason.";
constexpr std::string_view kColdLast = "Eileen: Your bag is on your desk.";
constexpr std::string_view kEnd =
    "narrate: The end. Open scripts/main.lua to write your own story.";

// The lines of the presentation demo, in order (templates/default/scripts/presentation.lua).
constexpr std::string_view kDemoFirst = "Eileen: This is the presentation demo.";
constexpr std::string_view kDemoFace = "Eileen: A new face fades in over the old one.";
constexpr std::string_view kDemoCamera = "Eileen: The camera pans and shakes. The backdrop stays.";
constexpr std::string_view kDemoSepia = "Eileen: A sepia shader on the backdrop layer fades in.";
constexpr std::string_view kDemoVoice = "Eileen: Now my mouth follows my voice.";
constexpr std::string_view kDemoMovie = "Eileen: A short movie follows.";
constexpr std::string_view kDemoLast =
    "Eileen: That was the presentation demo. Edit scripts/presentation.lua to change it.";
constexpr float kCameraPan = 80;  // camera { x = 0.0625 } on a screen 1280 wide
constexpr double kHalfSecond = 0.5;
constexpr double kLipStep = 0.01;
constexpr int kLipSteps = 250;  // 2.5 s: longer than the voice line

}  // namespace

TEST_CASE("template: new makes a game whose title is the folder name") {
  SampleGame game{"title"};

  CHECK(game.config().title == "my_game");
  CHECK(game.config().resolution == vnmaki::Resolution{.width = 1280, .height = 720});
  CHECK(game.at() == "screen title");  // the default title screen
}

TEST_CASE("template: the first branch plays to the end") {
  SampleGame game{"warm"};
  game.start_story();

  REQUIRE(game.at() == kFirstLine);
  const auto* say = std::get_if<SayRequest>(game.runtime().request());
  REQUIRE(say != nullptr);
  CHECK(say->color == std::optional<std::uint32_t>{kEileenColor});
  game.advance();
  REQUIRE(game.at() == "menu");
  const auto* menu = std::get_if<vnmaki::MenuRequest>(game.runtime().request());
  REQUIRE(menu != nullptr);
  CHECK(menu->options.size() == kSampleMenuOptions);
  game.choose(0);
  CHECK(game.at() == kWarmLine);
  game.advance();
  CHECK(game.at() == kWarmLast);
  game.advance();
  CHECK(game.at() == kEnd);
  game.advance();
  CHECK(game.at() == "ended");
}

TEST_CASE("template: the second branch plays to the end") {
  SampleGame game{"cold"};
  game.start_story();

  game.advance();
  REQUIRE(game.at() == "menu");
  game.choose(1);
  CHECK(game.at() == kColdLine);
  game.advance();
  CHECK(game.at() == kColdLast);
  game.advance();
  CHECK(game.at() == kEnd);
  game.advance();
  CHECK(game.at() == "ended");
}

TEST_CASE("template: roll back to the menu, take the other branch, save, load and roll back") {
  SampleGame game{"save"};
  game.start_story();
  game.advance();
  game.choose(0);
  REQUIRE(game.at() == kWarmLine);

  // Back over the choice, and the other answer: a new branch.
  REQUIRE(game.roll_back());
  CHECK(game.at() == "menu");
  game.choose(1);
  CHECK(game.at() == kColdLine);

  REQUIRE(game.saves().save("1").empty());
  game.advance();
  CHECK(game.at() == kColdLast);

  CHECK(game.load("1").empty());
  CHECK(game.at() == kColdLine);
  game.advance();
  CHECK(game.at() == kColdLast);
  REQUIRE(game.roll_back());  // the story after a load rolls back too
  CHECK(game.at() == kColdLine);
  game.advance();
  game.advance();
  CHECK(game.at() == kEnd);
  game.advance();
  CHECK(game.at() == "ended");
}

TEST_CASE("template: the auto save from the project's auto_save list is written") {
  SampleGame game{"auto"};
  CHECK(game.saves().exists(vnmaki::kAutoSlot));  // written at the title screen
  game.start_story();
  game.advance();

  REQUIRE(game.at() == "menu");  // the menu writes it again
  const std::vector<vnmaki::SlotInfo> slots = game.saves().list();
  REQUIRE(slots.size() == 1);
  CHECK(slots.front().slot == vnmaki::kAutoSlot);
  CHECK(slots.front().label == "classroom");
}

TEST_CASE("template: the font draws the text of the sample") {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(default_template())));
  const vnmaki::Font font = vnmaki::load_font(vfs, kFontPath);

  const std::string line{kWarmLine};
  const vnmaki::TextLayout layout = vnmaki::layout_text(font, line, kFontSize, kNoWrap);
  vnmaki::GlyphAtlas atlas{font};

  CHECK(layout.glyph_count == line.size());
  CHECK(atlas.prepare(layout));
}

TEST_CASE("template: the assets are the files of tools/generators/template_assets.py") {
  const fs::path generated = fs::path{VNMAKI_FIXTURES_DIR} / "template";
  std::size_t count = 0;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(generated)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const fs::path relative = fs::relative(entry.path(), generated);
    INFO(relative.string());
    REQUIRE(fs::exists(default_template() / relative));
    if (relative.extension() != ".png") {  // zlib versions compress the same pixels differently
      CHECK(read_file(entry.path()) == read_file(default_template() / relative));
    }
    ++count;
  }
  CHECK(count == kAssetCount);

  std::size_t committed = 0;
  for (const fs::directory_entry& entry : fs::recursive_directory_iterator(default_template())) {
    const std::string folder = fs::relative(entry.path(), default_template()).begin()->string();
    if (entry.is_regular_file() &&
        (folder == "images" || folder == "audio" || folder == "fonts" || folder == "shaders" ||
         folder == "models")) {
      ++committed;
    }
  }
  CHECK(committed == kAssetCount);
}

TEST_CASE("template: the movie is the committed test clip") {
  CHECK(read_file(default_template() / "movies/clip.webm") ==
        read_file(fs::path{VNMAKI_VIDEO_FIXTURES_DIR} / "clip.webm"));
}

namespace {

// Plays from the title to the menu and takes the third option: the presentation demo.
void start_presentation(SampleGame& game) {
  game.start_story();
  game.advance();
  REQUIRE(game.at() == "menu");
  game.choose(kPresentationOption);
}

[[nodiscard]] const vnmaki::LayeredSprite& eileen(SampleGame& game) {
  const auto* sprite = dynamic_cast<const vnmaki::LayeredSprite*>(game.scene().find("eileen"));
  REQUIRE(sprite != nullptr);
  return *sprite;
}

[[nodiscard]] std::string value_of(const vnmaki::LayeredSprite& sprite, std::string_view group) {
  for (const auto& [name, value] : sprite.attributes()) {
    if (name == group) {
      return value;
    }
  }
  return {};
}

[[nodiscard]] float sepia_amount(SampleGame& game) {
  return game.scene().layer_properties(vnmaki::Layer::kBackground).uniforms.at(0);
}

}  // namespace

TEST_CASE("template: the menu has a third option, the presentation demo, that plays to the end") {
  SampleGame game{"demo"};
  start_presentation(game);

  CHECK(game.at() == kDemoFirst);
  CHECK(eileen(game).attributes().size() == 5);  // base, outfit, face, lids, mouth
  CHECK(value_of(eileen(game), "face") == "smile");
  game.advance();
  CHECK(game.at() == kDemoFace);
  CHECK(value_of(eileen(game), "face") == "sad");
  game.advance();
  CHECK(game.at() == kDemoCamera);
  CHECK(game.scene().tweening("@camera"));  // the pan runs, and the next wait would end it
  game.advance();
  CHECK(game.at() == kDemoSepia);
  CHECK(game.scene().camera().pose.position.x == doctest::Approx(kCameraPan));
  CHECK(game.scene().tweening("@layer.background"));
  CHECK(sepia_amount(game) == doctest::Approx(0));
  game.step(kHalfSecond);
  CHECK(sepia_amount(game) == doctest::Approx(kHalfSecond));
  game.advance();
  CHECK(game.at() == kDemoVoice);
  CHECK(sepia_amount(game) == doctest::Approx(1));
  game.advance();
  CHECK(game.at() == kDemoMovie);
  game.advance();  // the movie is passed
  CHECK(game.at() == kDemoLast);
  CHECK(game.scene().camera().pose.position.x == doctest::Approx(0));
  game.advance();
  CHECK(game.at() == kEnd);
  game.advance();
  CHECK(game.at() == "ended");
}

TEST_CASE("template: the mouth of the sprite follows the voice line") {
  SampleGame game{"lips"};
  start_presentation(game);
  for (const std::string_view line : {kDemoFirst, kDemoFace, kDemoCamera, kDemoSepia}) {
    REQUIRE(game.at() == line);
    game.advance();
  }
  REQUIRE(game.at() == kDemoVoice);

  std::set<std::string> frames;
  for (int i = 0; i < kLipSteps; ++i) {
    frames.emplace(eileen(game).lip_frame());
    game.step(kLipStep);
  }
  CHECK(frames == std::set<std::string>{"closed", "half", "open"});
  CHECK(eileen(game).lip_frame() == "closed");  // the voice has ended
}

TEST_CASE("template: the blink plays on the sprite and the face crossfades") {
  SampleGame game{"blink"};
  start_presentation(game);
  REQUIRE(game.at() == kDemoFirst);

  CHECK(value_of(eileen(game), "lids") == "blink");  // it loops, so it never blocks a wait
  game.advance();
  CHECK(value_of(eileen(game), "face") == "sad");
  game.step(kHalfSecond);  // longer than the 0.25 s crossfade
  game.advance();
  CHECK(game.at() == kDemoCamera);
}

TEST_CASE("template: save during a tween, load, and roll back over the transitions") {
  SampleGame game{"demo_save"};
  start_presentation(game);
  REQUIRE(game.at() == kDemoFirst);

  // Back over the dissolve and the menu, and the same answer again.
  REQUIRE(game.roll_back());
  CHECK(game.at() == "menu");
  game.choose(kPresentationOption);
  REQUIRE(game.at() == kDemoFirst);

  for (const std::string_view line : {kDemoFirst, kDemoFace, kDemoCamera}) {
    REQUIRE(game.at() == line);
    game.advance();
  }
  REQUIRE(game.at() == kDemoSepia);
  game.step(kHalfSecond);
  REQUIRE(game.scene().tweening("@layer.background"));  // the tween runs while the game is saved
  REQUIRE(game.saves().save("1").empty());

  game.advance();
  CHECK(game.at() == kDemoVoice);
  CHECK(game.load("1").empty());
  CHECK(game.at() == kDemoSepia);
  CHECK(value_of(eileen(game), "face") == "sad");
  CHECK(game.scene().camera().pose.position.x == doctest::Approx(kCameraPan));
  CHECK_FALSE(game.scene().tweening("@layer.background"));  // a load leaves a tween at its end
  CHECK(sepia_amount(game) == doctest::Approx(1));

  // On to the mask transition, and back over it.
  game.advance();
  game.advance();
  REQUIRE(game.at() == kDemoMovie);
  game.advance();
  REQUIRE(game.at() == kDemoLast);
  REQUIRE(game.roll_back());
  CHECK(game.at() == kDemoMovie);
  game.advance();
  CHECK(game.at() == kDemoLast);
  game.advance();
  CHECK(game.at() == kEnd);
  game.advance();
  CHECK(game.at() == "ended");
}
