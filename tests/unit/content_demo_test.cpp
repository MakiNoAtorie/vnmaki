// The Phase 5 demo (docs/PLAN.md task 49), part 1: the label `content` of templates/default/ is
// played from packs, end to end. `vnmaki build windows` packs a copy of the template (the runner
// is a stand-in file, because the build only copies it), the copy is deleted, and the game plays
// from `game.vnpak` alone: the Lua plugin, the glTF model, the chime, the fonts and the
// translations all come out of the pack. Self-voicing is on with a recording backend (no voice, no
// window, no GPU). Each run checks the text that was spoken, the captions and the headless
// transcript. The GPU goldens are in tests/render/content_render_test.cpp.

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "cli/build_command.hpp"
#include "engine/audio/audio.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/playback.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/new_project.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/persistent_store.hpp"
#include "engine/scene/scene.hpp"
#include "engine/ui/preferences.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/self_voicing.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/recording_tts.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using Strings = std::vector<std::string>;

constexpr std::string_view kStartLabel = "content";
constexpr std::size_t kEnglish = 0;   // the menu option "English"
constexpr std::size_t kJapanese = 1;  // the menu option "Japanese"
constexpr std::size_t kLinesBeforeMenu = 2;
constexpr std::size_t kLinesAfterMenu = 7;  // six say lines and the closing narration
constexpr std::size_t kGlyphs = 20;
constexpr double kReveal = 100;  // seconds: the typewriter has finished

// The game of `vnmaki new`, packed by `vnmaki build windows` into a folder that only has the packs.
class PackedGame {
 public:
  PackedGame() : root_(vnmaki::test::unique_temp_path("vnmaki_content_demo_", "packed")) {
    fs::remove_all(root_);
    const fs::path project = root_ / "project";
    vnmaki::create_project(fs::path{VNMAKI_TEMPLATES_DIR} / "default", project, "content_demo");
    if (!vnmaki::shader_compiler_available()) {  // a release build cannot import the sepia shader
      const fs::path shaders = project / "shaders";
      fs::copy_file(fs::path{VNMAKI_PRECOMPILED_DIR} / "shaders" / "sepia.frag.vnshader",
                    shaders / "sepia.frag.vnshader");
      fs::remove(shaders / "sepia.frag.hlsl");
    }
    const fs::path runner = root_ / "runner.exe";
    std::ofstream{runner, std::ios::binary} << "not a program; the build only copies it";
    const std::string project_text = project.string();
    const std::string out_text = (root_ / "out").string();
    const std::string runner_text = runner.string();
    const std::vector<std::string_view> args{"windows", project_text, "--out",
                                             out_text,  "--runner",   runner_text};
    std::ostringstream out;
    std::ostringstream err;
    REQUIRE_MESSAGE(vnmaki::cli::build_command(args, out, err) == 0, err.str());
    fs::remove_all(project);  // only the packs can supply the game now
  }
  PackedGame(const PackedGame&) = delete;
  PackedGame& operator=(const PackedGame&) = delete;
  ~PackedGame() {
    std::error_code ignored;  // a folder left in the temp directory is not worth a failure
    fs::remove_all(root_, ignored);
  }

  [[nodiscard]] fs::path packs() const { return root_ / "out"; }
  [[nodiscard]] fs::path scratch(const std::string& name) const { return root_ / name; }

 private:
  fs::path root_;
};

const PackedGame& packed_game() {
  static const PackedGame game;
  return game;
}

// A game from the packs: a runtime with the scene and the audio of `vnmaki run`, the presenters of
// the story window, the preference applier for the language and self-voicing, and a recording TTS.
class Demo {
 public:
  Demo(const std::string& name, const std::string& start_language)
      : persistent_dir_(packed_game().scratch(name)),
        vfs_(mount_packs()),
        config_(vnmaki::load_project_config(vfs_)),
        scene_(vfs_, {.width = config_.resolution.width, .height = config_.resolution.height}),
        audio_(vfs_),
        state_({&scene_, &audio_}),
        translator_(vfs_, config_),
        runtime_(vfs_, runtime_options()),
        persistent_(runtime_, persistent_dir_, [](std::string_view) {}) {
    host_.set_bindings(&runtime_);
    persistent_.load();
    if (!start_language.empty()) {
      REQUIRE(translator_.set_language(start_language).empty());
    }
    runtime_.load_scripts();
    runtime_.start(std::string{kStartLabel});
    presenter_ = std::make_unique<vnmaki::DialoguePresenter>(
        runtime_, [](const vnmaki::DialogueLine&) { return kGlyphs; });
    presenter_->set_translator(&translator_);
    playback_ = std::make_unique<vnmaki::Playback>(runtime_, *presenter_, persistent_);
    screens_ = std::make_unique<vnmaki::ScreenPresenter>(runtime_, host_);
    screens_->set_translator(&translator_);
    voicing_ = std::make_unique<vnmaki::SelfVoicing>(tts, runtime_, *presenter_, *playback_, host_);
    voicing_->set_translator(&translator_);
    applier_ = std::make_unique<vnmaki::PreferenceApplier>(
        runtime_,
        vnmaki::PreferenceTargets{.self_voicing = voicing_.get(),
                                  .language = [this](const std::string& code) {
                                    apply_language(code);
                                  }});
    // The player turns self-voicing on in the preferences.
    REQUIRE(runtime_.set_binding("prefs", "self_voicing", true).empty());
    settle();
  }
  Demo(const Demo&) = delete;
  Demo& operator=(const Demo&) = delete;
  ~Demo() {
    std::error_code ignored;
    fs::remove_all(persistent_dir_, ignored);
  }

  // The frame of the story window that matters here: presenters, preferences, voicing, captions.
  void frame() {
    applier_->update();  // before the presenter, as in cli/story_window.cpp
    presenter_->update(kReveal);
    screens_->sync();
    voicing_->update();
    for (vnmaki::AudioCaption& caption : audio_.take_captions()) {
      captions.push_back(std::move(caption));
    }
  }

  // Ends the waits (the clip, the flash) as a click does, then shows the next request.
  void settle() {
    frame();
    while (const auto* wait = std::get_if<vnmaki::WaitRequest>(runtime_.request())) {
      scene_.finish_tweens(wait->tag);
      runtime_.advance();
      frame();
    }
  }
  void advance() {
    presenter_->advance();
    settle();
  }
  void choose(std::size_t index) {
    runtime_.choose(index);
    settle();
  }

  [[nodiscard]] bool finished() const {
    return runtime_.state() == vnmaki::RunState::kFinished;
  }
  [[nodiscard]] vnmaki::ScriptRuntime& runtime() { return runtime_; }
  [[nodiscard]] vnmaki::Scene& scene() { return scene_; }
  [[nodiscard]] const vnmaki::Translator& translator() const { return translator_; }

  vnmaki::test::RecordingTts tts;
  std::vector<vnmaki::AudioCaption> captions;

 private:
  vnmaki::Vfs mount_packs() {
    vnmaki::Vfs vfs;
    REQUIRE(vnmaki::mount_game_packs(vfs, packed_game().packs()));
    return vfs;
  }

  vnmaki::ScriptRuntimeOptions runtime_options() {
    vnmaki::ScriptRuntimeOptions options;
    options.screens = &host_;
    options.canvas_width = config_.resolution.width;
    options.canvas_height = config_.resolution.height;
    options.scene = &scene_;
    options.audio = &audio_;
    options.scene_state = &state_;
    options.languages = config_.languages;
    return options;
  }

  // What the story window does for a new language (cli/story_window.cpp).
  void apply_language(const std::string& code) {
    const std::string target = code.empty() ? translator_.source_language() : code;
    if (target != translator_.language()) {
      REQUIRE(translator_.set_language(target).empty());
    }
    scene_.set_language(target);
    audio_.set_language(target);
  }

  fs::path persistent_dir_;
  vnmaki::Vfs vfs_;
  vnmaki::ProjectConfig config_;
  vnmaki::Scene scene_;
  vnmaki::Audio audio_;
  vnmaki::CompositeSceneState state_;
  vnmaki::Translator translator_;
  vnmaki::UiHost host_{{.width = config_.resolution.width, .height = config_.resolution.height}};
  vnmaki::ScriptRuntime runtime_;
  vnmaki::PersistentStore persistent_;
  std::unique_ptr<vnmaki::DialoguePresenter> presenter_;
  std::unique_ptr<vnmaki::Playback> playback_;
  std::unique_ptr<vnmaki::ScreenPresenter> screens_;
  std::unique_ptr<vnmaki::SelfVoicing> voicing_;
  std::unique_ptr<vnmaki::PreferenceApplier> applier_;
};

// Plays the label to its end: the language menu takes `language`, every line is advanced.
void play(Demo& demo, std::size_t language) {
  for (std::size_t i = 0; i < kLinesBeforeMenu; ++i) {
    demo.advance();
  }
  demo.choose(language);
  for (std::size_t i = 0; i < kLinesAfterMenu; ++i) {
    demo.advance();
  }
}

// The headless transcript of the label in one language (the one the translator is in).
Strings transcript(const std::string& language, std::size_t choice) {
  vnmaki::Vfs vfs;
  REQUIRE(vnmaki::mount_game_packs(vfs, packed_game().packs()));
  const vnmaki::ProjectConfig config = vnmaki::load_project_config(vfs);
  vnmaki::Scene scene{vfs, {.width = config.resolution.width, .height = config.resolution.height}};
  vnmaki::Audio audio{vfs};
  vnmaki::CompositeSceneState state{{&scene, &audio}};
  vnmaki::Translator translator{vfs, config};
  REQUIRE(translator.set_language(language).empty());
  vnmaki::HeadlessOptions options;
  options.start_label = std::string{kStartLabel};
  options.translator = &translator;
  options.runtime.scene = &scene;
  options.runtime.audio = &audio;
  options.runtime.scene_state = &state;
  options.runtime.languages = config.languages;
  std::vector<vnmaki::ScriptInput> inputs(kLinesBeforeMenu, vnmaki::AdvanceInput{});
  inputs.emplace_back(vnmaki::ChooseInput{.index = choice});
  inputs.insert(inputs.end(), kLinesAfterMenu, vnmaki::AdvanceInput{});
  const vnmaki::HeadlessResult result = vnmaki::run_headless(vfs, inputs, options);
  CHECK_MESSAGE(result.outcome == vnmaki::HeadlessOutcome::kFinished, result.error);
  return result.transcript;
}

// The lines of the label in one language, in order (templates/default/scripts/content.lua and
// translations/ja.po). The story is the same in both languages, so one table type has both.
struct Texts {
  std::string_view intro, ask, english, japanese, same, captions, model, clip, plugin, last, end;
};

constexpr Texts kEnglishTexts{
    .intro = "This is the content demo: a language, captions, a 3D model and a plugin.",
    .ask = "First the language. Which one do you want?",
    .english = "English",
    .japanese = "Japanese",
    .same = "The story is the same in both languages. Prefs also has a language button.",
    .captions = "Captions name the sounds. A chime rang, and its caption is at the top left.",
    .model = "This is a 3D model, an arm with two joints.",
    .clip = "It plays the clip slide of its file.",
    .plugin = "A plugin adds the next transition.",
    .last = "That was the content demo. Edit scripts/content.lua to change it.",
    .end = "The end. Open scripts/main.lua to write your own story."};

constexpr Texts kJapaneseTexts{
    .intro = "これはコンテンツのデモです。言語、字幕、3Dモデル、プラグインを見せます。",
    .ask = "まず言語です。どちらにしますか？",
    .english = "英語",
    .japanese = "日本語",
    .same = "物語はどちらの言語でも同じです。設定にも言語のボタンがあります。",
    .captions = "字幕は音の名前を表示します。チャイムが鳴り、その字幕は左上にあります。",
    .model = "これは3Dモデルです。関節が二つある腕です。",
    .clip = "ファイルのクリップ slide を再生します。",
    .plugin = "次の場面転換はプラグインが追加したものです。",
    .last = "コンテンツのデモは以上です。scripts/content.lua を編集して変えてください。",
    .end = "おわり。あなたの物語を書こう。"};

std::string said(std::string_view text) {
  return "Eileen: " + std::string{text};
}

// What self-voicing says until the menu is answered: two lines and the options.
Strings spoken_to_menu(const Texts& texts) {
  return {said(texts.intro), said(texts.ask),
          "Choose. " + std::string{texts.english} + ". " + std::string{texts.japanese}};
}

// ... and after it: the lines of the second half, and the narration, which has no speaker.
Strings spoken_after_menu(const Texts& texts) {
  return {said(texts.same),   said(texts.captions), said(texts.model), said(texts.clip),
          said(texts.plugin), said(texts.last),     std::string{texts.end}};
}

Strings joined(Strings first, const Strings& second) {
  first.insert(first.end(), second.begin(), second.end());
  return first;
}

// The headless transcript, in the order of the script. `transition flash` is the transition of
// the Lua plugin in the pack.
Strings transcript_of(const Texts& texts) {
  return {"label content",
          "scene bg/classroom",
          "show eileen at center",
          "say " + said(texts.intro),
          "say " + said(texts.ask),
          "menu 1: " + std::string{texts.english},
          "menu 2: " + std::string{texts.japanese},
          "say " + said(texts.same),
          "sound sfx/chime",
          "say " + said(texts.captions),
          "hide eileen",
          "scene3d stage",
          "camera stage",
          "model arm",
          "say " + said(texts.model),
          "play @3d.stage.arm",
          "say " + said(texts.clip),
          "wait @3d.stage.arm",
          "hide stage",
          "say " + said(texts.plugin),
          "scene bg/rooftop",
          "show eileen at center",
          "transition flash",
          "wait @transition",
          "say " + said(texts.last),
          "narrate " + std::string{texts.end}};
}

constexpr double kChimeSeconds = 0.5;
constexpr double kChimeTolerance = 0.05;  // the Opus file has a little padding

void check_captions(const Demo& demo) {
  REQUIRE(demo.captions.size() == 1);
  CHECK(demo.captions.front().text == "a chime rings");  // a caption is not translated
  CHECK(demo.captions.front().seconds == doctest::Approx(kChimeSeconds).epsilon(kChimeTolerance));
}

}  // namespace

TEST_CASE("content: from packs in English, self-voicing speaks the lines and the caption shows") {
  Demo demo{"english", ""};
  REQUIRE(demo.translator().language() == "en");
  play(demo, kEnglish);

  CHECK_MESSAGE(demo.finished(), demo.runtime().error());
  CHECK(demo.tts.spoken ==
        joined(spoken_to_menu(kEnglishTexts), spoken_after_menu(kEnglishTexts)));
  CHECK(demo.tts.stops == 0);
  check_captions(demo);
  CHECK(vnmaki::read_preferences(demo.runtime()).captions);  // the script turned them on
  CHECK(vnmaki::read_preferences(demo.runtime()).language.empty());
}

TEST_CASE("content: choosing Japanese switches the language for the rest of the story") {
  Demo demo{"switch", ""};
  play(demo, kJapanese);

  CHECK_MESSAGE(demo.finished(), demo.runtime().error());
  CHECK(demo.translator().language() == "ja");
  CHECK(vnmaki::read_preferences(demo.runtime()).language == "ja");
  // The lines and the menu before the answer are English; the line after it is already Japanese.
  CHECK(demo.tts.spoken ==
        joined(spoken_to_menu(kEnglishTexts), spoken_after_menu(kJapaneseTexts)));
  check_captions(demo);
}

TEST_CASE("content: from packs in Japanese, the lines, the menu and the narration are Japanese") {
  Demo demo{"japanese", "ja"};
  REQUIRE(demo.translator().language() == "ja");
  play(demo, kJapanese);

  CHECK_MESSAGE(demo.finished(), demo.runtime().error());
  CHECK(demo.tts.spoken ==
        joined(spoken_to_menu(kJapaneseTexts), spoken_after_menu(kJapaneseTexts)));
  check_captions(demo);
}

TEST_CASE("content: the headless transcript from packs, in both languages") {
  // Same commands in both: the 3D model, its clip and the transition of the plugin all ran.
  CHECK(transcript("en", kEnglish) == transcript_of(kEnglishTexts));
  CHECK(transcript("ja", kJapanese) == transcript_of(kJapaneseTexts));
}

