// Runtime languages (docs/SPEC.md section 16, ticket 41): the translations by the keys that
// `vnmaki extract-strings` writes, a headless transcript in two languages, the image and voice
// folders of a language, a switch during a line, screens and menus, the style override of a
// language and a right-to-left hbox. Expected strings are written out by hand.

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/audio/audio.hpp"
#include "engine/dialogue/backlog.hpp"
#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/l10n/extract.hpp"
#include "engine/l10n/po.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/runtime/headless_runner.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/scene/scene.hpp"
#include "engine/ui/backlog_screen.hpp"
#include "engine/ui/preferences.hpp"
#include "engine/ui/screen_presenter.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_layout.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/audio_fixtures.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::ChooseInput;
using vnmaki::PoEntry;
using vnmaki::PoFile;
using vnmaki::TextDirection;
using vnmaki::Translator;
using Files = std::map<std::string, std::string, std::less<>>;
using Lines = std::vector<std::string>;

constexpr double kQuarterStep = 0.0625;  // a binary fraction, so the clock adds up exactly
constexpr std::size_t kFiveGlyphs = 5;
constexpr double kSecondsToFinish = 100;
constexpr float kCellWidth = 40;
constexpr float kWideCell = 100;
constexpr float kMediumCell = 50;
constexpr float kNarrowCell = 25;
constexpr float kBodySize = 20;  // the sizes of the styles in the style test
constexpr float kTitleSize = 50;
constexpr float kOverrideSize = 40;
constexpr float kCellHeight = 10;
constexpr vnmaki::Size kCanvas{.width = 300, .height = 200};
const vnmaki::Viewport kViewport =
    vnmaki::compute_viewport(kCanvas, kCanvas, vnmaki::ScaleMode::kFit);

constexpr std::string_view kProject = R"(title = "T"
languages = ["en", "ja", "ar"]

[text_direction]
ar = "rtl"

[styles]
ja = "styles/ja_look.lua"
)";

// A `.po` entry as `vnmaki extract-strings` writes it. `speaker` is the token of the script.
PoEntry entry(std::string_view scope, std::string_view speaker, std::string text,
              std::string translation, bool fuzzy = false, bool obsolete = false) {
  PoEntry result;
  result.context = vnmaki::translation_key(scope, speaker, text);
  result.id = std::move(text);
  result.str = std::move(translation);
  if (!speaker.empty()) {
    result.extracted.push_back("speaker: " + std::string{speaker});
  }
  result.set_fuzzy(fuzzy);
  result.obsolete = obsolete;
  return result;
}

std::string po_text(std::vector<PoEntry> entries) {
  PoFile file;
  file.entries.push_back(vnmaki::po_header("ja"));
  for (PoEntry& item : entries) {
    file.entries.push_back(std::move(item));
  }
  return vnmaki::write_po(file);
}

constexpr auto kStory = R"(
local eileen = character { name = "Eileen" }
label("start", function()
  say(eileen, "Hello.")
  narrate("Silence.")
  local choice = menu { "Yes", "No" }
  say("Bob", "Bye.")
end)
screen("greeting", function()
  return vbox { text { "Welcome" }, button { "Start", returns = "start" } }
end)
label("page", function()
  show_screen("greeting")
  say(eileen, "Hello.")
  menu { "Yes", "No" }
  say(eileen, "Hello.")
end)
)";

std::string ja_po() {
  return po_text({
      entry("start", "eileen", "Hello.", "Konnichiwa."),
      entry("start", "", "Silence.", "Shizukesa.", true),  // fuzzy: the source text shows
      entry("start", "", "Yes", "Hai"),                    // "No" has no entry
      entry("start", "Bob", "Bye.", "Jaa.", false, true),  // obsolete: the source text shows
      entry("page", "eileen", "Hello.", "Konnichiwa (page)."),
      entry("page", "", "Yes", "Hai (page)"),
      entry("screen/greeting", "", "Welcome", "Youkoso"),
      entry("screen/greeting", "", "Start", "Hajimeru"),
  });
}

Files game_files(const std::string& script = kStory) {
  return {{"project.toml", std::string{kProject}},
          {"scripts/main.lua", script},
          {"translations/ja.po", ja_po()}};
}

vnmaki::ProjectConfig config_of(const vnmaki::Vfs& vfs) { return vnmaki::load_project_config(vfs); }

}  // namespace

TEST_CASE("language: a catalog finds the key of the speaker, then the speaker's name") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(game_files());
  Translator translator{vfs, config_of(vfs)};
  REQUIRE(translator.set_language("ja").empty());
  // The script wrote `eileen`; the runtime only knows the name `Eileen`.
  CHECK(translator.translate("start", "eileen", "Hello.") == "Konnichiwa.");
  CHECK(translator.translate("start", "Eileen", "Hello.") == "Konnichiwa.");
  CHECK(translator.translate("start", "", "Yes") == "Hai");
  CHECK(translator.translate("screen/greeting", "", "Welcome") == "Youkoso");
  // The scope is part of the key.
  CHECK(translator.translate("page", "Eileen", "Hello.") == "Konnichiwa (page).");
  CHECK(translator.translate("other", "Eileen", "Hello.") == "Hello.");
  // No entry, a fuzzy entry and an obsolete entry give the source text.
  CHECK(translator.translate("start", "", "No") == "No");
  CHECK(translator.translate("start", "", "Silence.") == "Silence.");
  CHECK(translator.translate("start", "Bob", "Bye.") == "Bye.");
  // A line that has no speaker never takes the entry of a speaker with the same text.
  CHECK(translator.translate("start", "", "Hello.") == "Hello.");
}

TEST_CASE("language: the translator switches language, keeps a revision and reads project.toml") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs([] {
    Files files = game_files();
    files["styles/ar.lua"] = "";
    return files;
  }());
  Translator translator{vfs, config_of(vfs)};
  CHECK(translator.language() == "en");
  CHECK(translator.source_language() == "en");
  CHECK(translator.direction() == TextDirection::kLtr);
  CHECK(translator.translate("start", "", "Yes") == "Yes");

  const std::uint64_t before = translator.revision();
  CHECK(translator.set_language("ja").empty());
  CHECK(translator.revision() != before);
  CHECK(translator.style_file() == "styles/ja_look.lua");
  CHECK(translator.set_language("ar").empty());  // no ar.po: no translations
  CHECK(translator.direction() == TextDirection::kRtl);
  CHECK(translator.style_file() == "styles/ar.lua");  // the default name, as the file exists
  CHECK(translator.translate("start", "", "Yes") == "Yes");

  const std::uint64_t at_arabic = translator.revision();
  CHECK(translator.set_language("xx") == "language 'xx' is not in 'languages' of project.toml");
  CHECK(translator.language() == "ar");
  CHECK(translator.revision() == at_arabic);
  CHECK(translator.set_language("en").empty());
  CHECK(translator.direction() == TextDirection::kLtr);
  CHECK(translator.style_file().empty());
}

TEST_CASE("language: a .po file that does not parse leaves the language with no translations") {
  Files files = game_files();
  files["translations/ja.po"] = "msgid \"a\"\nmsgstr \"b\nmsgid";
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(files);
  Translator translator{vfs, config_of(vfs)};
  const std::string problem = translator.set_language("ja");
  CHECK(problem.starts_with("translations/ja.po:"));
  CHECK(translator.language() == "ja");
  CHECK(translator.translate("start", "", "Yes") == "Yes");
}

TEST_CASE("language: project.toml reads the text direction and the style file of a language") {
  const vnmaki::ProjectConfig config = vnmaki::parse_project_config(kProject);
  CHECK(config.text_directions.at("ar") == TextDirection::kRtl);
  CHECK(config.language_styles.at("ja") == "styles/ja_look.lua");

  const auto fails = [](std::string_view text) {
    return vnmaki::parse_project_config("title = \"T\"\nlanguages = [\"en\", \"ar\"]\n" +
                                        std::string{text});
  };
  CHECK_THROWS_WITH_AS(fails("[text_direction]\nar = \"up\"\n"),
                       doctest::Contains("'text_direction.ar' must be \"ltr\" or \"rtl\""),
                       vnmaki::ProjectConfigError);
  CHECK_THROWS_WITH_AS(fails("[text_direction]\nde = \"rtl\"\n"),
                       doctest::Contains("'text_direction' has language 'de'"),
                       vnmaki::ProjectConfigError);
  CHECK_THROWS_WITH_AS(fails("[styles]\nar = \"\"\n"),
                       doctest::Contains("'styles.ar' must be a file path"),
                       vnmaki::ProjectConfigError);
  CHECK_THROWS_AS(fails("styles = 3\n"), vnmaki::ProjectConfigError);
}

TEST_CASE("language: the headless transcript is the same story in two languages") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(game_files());
  Translator translator{vfs, config_of(vfs)};
  const std::vector<vnmaki::ScriptInput> inputs{vnmaki::AdvanceInput{}, vnmaki::AdvanceInput{},
                                                ChooseInput{.index = 0}, vnmaki::AdvanceInput{}};
  vnmaki::HeadlessOptions options;
  options.translator = &translator;

  const vnmaki::HeadlessResult english = vnmaki::run_headless(vfs, inputs, options);
  REQUIRE(english.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(english.transcript == Lines{"label start", "say Eileen: Hello.", "narrate Silence.",
                                    "menu 1: Yes", "menu 2: No", "say Bob: Bye."});

  REQUIRE(translator.set_language("ja").empty());
  const vnmaki::HeadlessResult japanese = vnmaki::run_headless(vfs, inputs, options);
  REQUIRE(japanese.outcome == vnmaki::HeadlessOutcome::kFinished);
  CHECK(japanese.transcript == Lines{"label start", "say Eileen: Konnichiwa.", "narrate Silence.",
                                     "menu 1: Hai", "menu 2: No", "say Bob: Bye."});
  // The story is the same: the positions of the yield points do not depend on the language.
  CHECK(japanese.stops == english.stops);
}

TEST_CASE("language: an image is looked up in the folder of the language first") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({
      {"images/bg/a.png", ""},
      {"images/ja/bg/a.png", ""},
      {"images/bg/b.png", ""},
      {"images/ja/bg/c.webp", ""},
  });
  CHECK(vnmaki::resolve_image(vfs, "bg/a") == "images/bg/a.png");
  CHECK(vnmaki::resolve_image(vfs, "bg/a", "ja") == "images/ja/bg/a.png");
  CHECK(vnmaki::resolve_image(vfs, "bg/a", "en") == "images/bg/a.png");
  CHECK(vnmaki::resolve_image(vfs, "bg/b", "ja") == "images/bg/b.png");  // no ja version
  CHECK(vnmaki::resolve_image(vfs, "bg/c", "ja") == "images/ja/bg/c.webp");
  CHECK_FALSE(vnmaki::resolve_image(vfs, "bg/c", "en"));
  CHECK_FALSE(vnmaki::resolve_image(vfs, "bg/c"));

  vnmaki::Scene scene{vfs, kCanvas};
  const auto shown_file = [&](const char* tag) {
    const auto* image = dynamic_cast<const vnmaki::ImageDisplayable*>(scene.find(tag));
    REQUIRE(image != nullptr);
    return image->path();
  };
  REQUIRE(scene.show({.tag = "one", .image = "bg/a"}).empty());
  scene.set_language("ja");
  REQUIRE(scene.show({.tag = "two", .image = "bg/a"}).empty());
  REQUIRE(scene.show({.tag = "three", .image = "bg/b"}).empty());
  CHECK(shown_file("one") == "images/bg/a.png");  // shown before the change
  CHECK(shown_file("two") == "images/ja/bg/a.png");
  CHECK(shown_file("three") == "images/bg/b.png");
}

TEST_CASE("language: a voice is looked up in the folder of the language first") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({
      {"audio/voice/eileen/001.opus", vnmaki::test::audio_fixture("tone.opus")},
      {"audio/voice/ja/eileen/001.opus", vnmaki::test::audio_fixture("tone.opus")},
      {"audio/voice/ja/eileen/002.opus", vnmaki::test::audio_fixture("tone.opus")},
  });
  CHECK(vnmaki::voice_audio_name(vfs, "eileen/001", "") == "voice/eileen/001");
  CHECK(vnmaki::voice_audio_name(vfs, "eileen/001", "ja") == "voice/ja/eileen/001");
  CHECK(vnmaki::voice_audio_name(vfs, "eileen/001", "en") == "voice/eileen/001");
  CHECK(vnmaki::voice_audio_name(vfs, "eileen/003", "ja") == "voice/eileen/003");

  vnmaki::Audio audio{vfs};
  CHECK_FALSE(audio.voice("eileen/002").empty());  // only the ja file exists
  audio.set_language("ja");
  CHECK(audio.voice("eileen/002").empty());
  CHECK(audio.voice("eileen/001").empty());
  audio.set_language("en");
  CHECK_FALSE(audio.voice("eileen/002").empty());
}

namespace {

// A story that waits at its first line, with the presenter, the backlog and a translator.
struct Session {
  explicit Session(const char* label = "start")
      : vfs(vnmaki::test::memory_vfs(game_files())),
        translator(vfs, config_of(vfs)),
        runtime(vfs, {.languages = {"en", "ja", "ar"}}),
        presenter(runtime, [](const vnmaki::DialogueLine& line) { return line.text.size(); }),
        backlog(runtime, nullptr, vnmaki::kDefaultBacklogLength, &translator) {
    runtime.load_scripts();
    runtime.start(label);
    presenter.set_translator(&translator);
    presenter.update(0);
  }

  [[nodiscard]] std::string shown() const { return presenter.line() ? presenter.line()->text : ""; }

  vnmaki::Vfs vfs;
  Translator translator;
  vnmaki::ScriptRuntime runtime;
  vnmaki::DialoguePresenter presenter;
  vnmaki::Backlog backlog;
};

// Shows the whole line, then moves the story on.
void next_line(Session& session) {
  session.presenter.update(kSecondsToFinish);
  session.presenter.advance();
}

}  // namespace

TEST_CASE("language: a switch during a line changes the text and goes on revealing") {
  Session session;
  REQUIRE(session.shown() == "Hello.");
  session.presenter.update(kQuarterStep);  // 40 characters a second: 2.5 glyphs
  const std::size_t visible = session.presenter.visible_glyphs();
  REQUIRE(visible == 2);
  const std::uint64_t serial = session.presenter.line_serial();
  const std::size_t steps = session.runtime.rollback_steps().size();
  const vnmaki::ScriptPosition position = session.runtime.position();

  REQUIRE(session.translator.set_language("ja").empty());
  session.presenter.update(0);
  CHECK(session.shown() == "Konnichiwa.");
  CHECK(session.presenter.glyph_count() == std::string{"Konnichiwa."}.size());
  CHECK(session.presenter.visible_glyphs() == visible);  // the reveal did not start again
  CHECK(session.presenter.line_serial() == serial);      // the same line, so no new "seen" mark
  CHECK(session.presenter.revealing());

  session.presenter.update(kQuarterStep);
  CHECK(session.presenter.visible_glyphs() == kFiveGlyphs);

  // Back again, with the line finished: it stays finished.
  session.presenter.advance();
  REQUIRE_FALSE(session.presenter.revealing());
  REQUIRE(session.translator.set_language("en").empty());
  session.presenter.update(0);
  CHECK(session.shown() == "Hello.");
  CHECK_FALSE(session.presenter.revealing());
  CHECK(session.presenter.visible_glyphs() == session.presenter.glyph_count());

  // A change of language is not a step: the story did not move.
  CHECK(session.runtime.position() == position);
  CHECK(session.runtime.rollback_steps().size() == steps);
}

TEST_CASE("language: the backlog and the backlog screen follow the language") {
  Session session;
  next_line(session);         // "Silence." (a fuzzy entry)
  next_line(session);         // the menu
  session.runtime.choose(0);  // "Bye."
  vnmaki::UiHost host{kCanvas};
  vnmaki::BacklogScreen screen{host, session.backlog};
  screen.open();
  const auto texts = [&] {
    Lines result;
    for (const vnmaki::BacklogEntry& item : screen.entries()) {
      result.push_back(item.text);
    }
    return result;
  };
  CHECK(texts() == Lines{"Hello.", "Silence.", "Bye."});

  REQUIRE(session.translator.set_language("ja").empty());
  screen.sync();
  CHECK(texts() == Lines{"Konnichiwa.", "Silence.", "Bye."});
  CHECK(screen.selected() == 2);  // the selected line stays
  CHECK(session.backlog.entries().front().text == "Konnichiwa.");
  // The entries of the runtime are the source text: a rollback restores the same story.
  const auto* say =
      std::get_if<vnmaki::SayRequest>(&session.runtime.rollback_steps().front().request);
  REQUIRE(say != nullptr);
  CHECK(say->text == "Hello.");

  REQUIRE(session.translator.set_language("en").empty());
  screen.sync();
  CHECK(texts() == Lines{"Hello.", "Silence.", "Bye."});
}

TEST_CASE("language: a menu and a screen are translated, also when the language changes") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(game_files());
  Translator translator{vfs, config_of(vfs)};
  vnmaki::UiHost host{kCanvas};
  host.set_translator(&translator);
  vnmaki::ScriptRuntime runtime{vfs, {.screens = &host}};
  runtime.load_scripts();
  runtime.start("page");
  vnmaki::ScreenPresenter screens{runtime, host};
  screens.set_translator(&translator);

  const auto button_texts = [&](std::string_view screen) {
    Lines result;
    const vnmaki::UiNode* root = host.find(screen);
    REQUIRE(root != nullptr);
    const auto walk = [&](const auto& self, const vnmaki::UiNode& node) -> void {
      if (node.kind == vnmaki::NodeKind::kText || node.kind == vnmaki::NodeKind::kButton) {
        result.push_back(node.text);
      }
      for (const vnmaki::UiNode& child : node.children) {
        self(self, child);
      }
    };
    walk(walk, *root);
    return result;
  };

  CHECK(button_texts("greeting") == Lines{"Welcome", "Start"});
  REQUIRE(translator.set_language("ja").empty());
  CHECK(button_texts("greeting") == Lines{"Youkoso", "Hajimeru"});  // at once
  const vnmaki::UiNode* start = host.find("greeting");
  REQUIRE(start != nullptr);
  CHECK(start->children[1].label == "Hajimeru");
  CHECK(start->children[1].source_text == "Start");

  runtime.advance();  // past the line, at the menu
  screens.sync();
  CHECK(button_texts(std::string{vnmaki::kMenuScreenName}) == Lines{"Hai (page)", "No"});
  REQUIRE(translator.set_language("en").empty());
  screens.sync();
  CHECK(button_texts(std::string{vnmaki::kMenuScreenName}) == Lines{"Yes", "No"});
  CHECK(button_texts("greeting") == Lines{"Welcome", "Start"});
  CHECK(runtime.position().step == 2);  // the story did not move
}

TEST_CASE("language: the style file of a language runs over the game's styles and goes away") {
  constexpr auto kStyled = R"(
style("body", { size = 20 })
style("title", { size = 50 })
screen("s", function()
  return vbox { text { "A", style = "body" }, text { "B", style = "title" } }
end)
label("start", function()
  show_screen("s")
  say("X", "line")
  hide_screen("s")
  show_screen("s")
  say("X", "again")
end)
)";
  Files files{{"scripts/main.lua", kStyled},
              {"styles/ja_look.lua", "style('body', { size = 40 })\n"},
              {"styles/broken.lua", "style('body', { size = 40 }) error('boom')\n"}};
  vnmaki::Vfs vfs = vnmaki::test::memory_vfs(files);
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime{vfs, {.screens = &host}};
  host.set_bindings(&runtime);
  // Before the scripts load it only remembers the file; load_scripts() runs it after the game's.
  CHECK(runtime.set_language_styles("styles/ja_look.lua").empty());
  runtime.load_scripts();
  runtime.start("start");

  const auto sizes = [&] {
    const vnmaki::UiNode* root = host.find("s");
    REQUIRE(root != nullptr);
    return std::pair{root->children[0].style.text_size, root->children[1].style.text_size};
  };
  CHECK(sizes() == std::pair{kOverrideSize, kTitleSize});

  CHECK(runtime.set_language_styles("").empty());  // back to the game's styles
  runtime.advance();                               // hide, show again, at the second line
  CHECK(sizes() == std::pair{kBodySize, kTitleSize});
  CHECK(runtime.set_language_styles("styles/ja_look.lua").empty());
  runtime.advance();  // the story ends; the screen stays as it was built
  CHECK(sizes() == std::pair{kBodySize, kTitleSize});

  // A missing file and a file that fails are reported; the game's styles stay.
  CHECK(runtime.set_language_styles("styles/none.lua").find("not found") != std::string::npos);
  CHECK(runtime.set_language_styles("styles/broken.lua").find("boom") != std::string::npos);
  CHECK(runtime.set_language_styles("styles/ja_look.lua").empty());  // and it still works
}

TEST_CASE("language: prefs.language is read by the applier, which tells the language target") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(game_files());
  vnmaki::ScriptRuntime runtime{vfs, {.languages = {"en", "ja", "ar"}}};
  runtime.load_scripts();
  CHECK(vnmaki::read_preferences(runtime).language.empty());
  CHECK(runtime.eval("#languages()").text == "3");
  CHECK(runtime.eval("languages()[2]").text == "ja");

  Lines heard;
  vnmaki::PreferenceApplier applier{
      runtime, {.language = [&](const std::string& code) { heard.push_back(code); }}};
  applier.update();
  CHECK(heard.empty());  // the source language is the default
  CHECK(runtime.set_binding("prefs", "language", std::string{"ja"}).empty());
  applier.update();
  applier.update();
  CHECK(heard == Lines{"ja"});  // told once
  CHECK(vnmaki::read_preferences(runtime).language == "ja");
  CHECK_FALSE(runtime.set_binding("prefs", "language", true).empty());  // a string only
  CHECK(runtime.set_binding("prefs", "language", std::string{}).empty());
  applier.update();
  CHECK(heard == Lines{"ja", ""});
}

TEST_CASE("language: the preferences screen has a button for each language") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(game_files());
  vnmaki::UiHost host{kCanvas};
  vnmaki::ScriptRuntime runtime{vfs, {.screens = &host, .languages = {"en", "ja"}}};
  runtime.load_scripts();
  vnmaki::ScreenNodeDef tree;
  REQUIRE(runtime.build_screen("preferences", {}, tree).empty());
  Lines returns;
  const auto walk = [&](const auto& self, const vnmaki::ScreenNodeDef& node) -> void {
    if (node.kind == "button") {
      for (const auto& [name, value] : node.props) {
        if (name == "returns") {
          returns.push_back(std::get<std::string>(value));
        }
      }
    }
    for (const vnmaki::ScreenNodeDef& child : node.children) {
      self(self, child);
    }
  };
  walk(walk, tree);
  CHECK(returns == Lines{"@language:en", "@language:ja", "@close"});

  // A game with one language has no such row.
  vnmaki::ScriptRuntime single{vfs, {.languages = {"en"}}};
  single.load_scripts();
  tree = {};
  REQUIRE(single.build_screen("preferences", {}, tree).empty());
  returns.clear();
  walk(walk, tree);
  CHECK(returns == Lines{"@close"});
}

TEST_CASE("language: a right-to-left hbox puts its first child at the right edge") {
  const auto row = [] {
    vnmaki::UiNode box = vnmaki::make_node(vnmaki::NodeKind::kHbox);
    for (const float width : {kWideCell, kMediumCell, kNarrowCell}) {
      vnmaki::UiNode cell = vnmaki::make_node(vnmaki::NodeKind::kBox);
      cell.style.width = width;
      cell.style.height = kCellHeight;
      box.children.push_back(cell);
    }
    vnmaki::UiNode root = vnmaki::make_node(vnmaki::NodeKind::kBox);
    root.children.push_back(box);
    return root;
  };
  const auto xs = [](const vnmaki::UiNode& root) {
    std::vector<float> result;
    for (const vnmaki::UiNode& cell : root.children[0].children) {
      result.push_back(cell.rect.x);
    }
    return result;
  };
  vnmaki::UiNode ltr = row();
  vnmaki::layout_tree(ltr, kCanvas, vnmaki::estimate_text_size);
  CHECK(xs(ltr) == std::vector<float>{0, 100, 150});

  vnmaki::UiNode rtl = row();
  vnmaki::layout_tree(rtl, kCanvas, vnmaki::estimate_text_size, TextDirection::kRtl);
  CHECK(xs(rtl) == std::vector<float>{200, 150, 125});
  CHECK(rtl.children[0].children[0].rect.width == 100);
  // A vbox does not change.
  vnmaki::UiNode column = vnmaki::make_node(vnmaki::NodeKind::kVbox);
  for (int i = 0; i < 2; ++i) {
    vnmaki::UiNode cell = vnmaki::make_node(vnmaki::NodeKind::kBox);
    cell.style.height = kCellHeight;
    column.children.push_back(cell);
  }
  vnmaki::layout_tree(column, kCanvas, vnmaki::estimate_text_size, TextDirection::kRtl);
  CHECK(column.children[0].rect.y == 0);
  CHECK(column.children[1].rect.y == 10);
}

TEST_CASE("language: UiHost lays screens out right to left when the language says so") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(game_files());
  Translator translator{vfs, config_of(vfs)};
  vnmaki::UiHost host{kCanvas};
  host.set_translator(&translator);
  vnmaki::UiNode row = vnmaki::make_node(vnmaki::NodeKind::kHbox);
  for (int i = 0; i < 2; ++i) {
    vnmaki::UiNode cell = vnmaki::make_node(vnmaki::NodeKind::kBox);
    cell.style.width = kCellWidth;
    cell.style.height = kCellHeight;
    row.children.push_back(cell);
  }
  host.show("row", std::move(row), false);
  REQUIRE(host.find("row") != nullptr);
  CHECK(host.find("row")->children[0].rect.x == 0);
  REQUIRE(translator.set_language("ar").empty());
  CHECK(host.find("row")->children[0].rect.x == 260);  // 300 - 40
  CHECK(host.find("row")->children[1].rect.x == 220);
  REQUIRE(translator.set_language("en").empty());
  CHECK(host.find("row")->children[0].rect.x == 0);
}
