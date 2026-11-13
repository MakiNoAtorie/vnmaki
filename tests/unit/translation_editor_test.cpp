// The translation editor of the dev overlay (docs/SPEC.md section 16): the entries of the current
// label, an edit that reaches the say box at once and is saved to the `.po` file. The tests use a
// real temporary project folder (the editor writes files); no window or GPU is needed.

#include "engine/tools/translation_editor.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/l10n/extract.hpp"
#include "engine/l10n/po.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

using vnmaki::TranslationRow;

constexpr auto kProject = "title = \"T\"\nlanguages = [\"en\", \"ja\"]\n";
constexpr auto kScript = R"(
label("start", function()
  say("Eileen", "Hello.")
  say("Eileen", "Plain {b}bold{/b}.")
end)
label("other", function()
  say("Eileen", "Elsewhere.")
end)
)";

namespace fs = std::filesystem;

struct Project {
  Project() : dir(vnmaki::test::unique_temp_path("vnmaki_", "translation_editor")) {
    fs::remove_all(dir);
    fs::create_directories(dir / "scripts");
    fs::create_directories(dir / "translations");
    write("project.toml", kProject);
    write("scripts/main.lua", kScript);
    vnmaki::PoFile file;
    file.entries.push_back(vnmaki::po_header("ja"));
    file.entries.push_back(entry("start", "Hello.", "Konnichiwa.", false));
    file.entries.push_back(entry("start", "Plain {b}bold{/b}.", "Futsuu {b}futoji{/b}.", true));
    file.entries.push_back(entry("other", "Elsewhere.", "", false));
    write("translations/ja.po", vnmaki::write_po(file));
    REQUIRE(vfs.mount(vnmaki::make_directory_mount(dir)));
  }
  Project(const Project&) = delete;
  Project& operator=(const Project&) = delete;
  ~Project() {
    std::error_code ignored;
    fs::remove_all(dir, ignored);
  }

  static vnmaki::PoEntry entry(const std::string& scope, const std::string& text,
                               const std::string& translation, bool fuzzy) {
    vnmaki::PoEntry result;
    result.context = vnmaki::translation_key(scope, "Eileen", text);
    result.id = text;
    result.str = translation;
    result.extracted.push_back("speaker: Eileen");
    result.set_fuzzy(fuzzy);
    return result;
  }

  void write(const std::string& path, const std::string& text) const {
    std::ofstream out{dir / path, std::ios::binary};
    out << text;
  }

  [[nodiscard]] std::string read(const std::string& path) const {
    std::ifstream in{dir / path, std::ios::binary};
    return {std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  }

  fs::path dir;
  vnmaki::Vfs vfs;
};

}  // namespace

TEST_CASE("translation editor: it lists the entries of the current label and saves an edit") {
  Project project;
  vnmaki::Translator translator{project.vfs, vnmaki::load_project_config(project.vfs)};
  vnmaki::ScriptRuntime runtime{project.vfs};
  runtime.load_scripts();
  runtime.start("start");
  vnmaki::DialoguePresenter presenter{
      runtime, [](const vnmaki::DialogueLine& line) { return line.text.size(); }};
  presenter.set_translator(&translator);
  vnmaki::TranslationEditor editor{translator, runtime, project.dir};

  // The source language has no `.po` file to edit.
  CHECK_FALSE(editor.editable());
  CHECK(editor.rows().empty());
  CHECK(editor.edit("any", "x") == "the source language has no translations");

  REQUIRE(translator.set_language("ja").empty());
  presenter.update(0);
  REQUIRE(editor.editable());
  const std::vector<TranslationRow> rows = editor.rows();
  REQUIRE(rows.size() == 2);  // the label `other` is not listed
  CHECK(rows[0] == TranslationRow{.key = vnmaki::translation_key("start", "Eileen", "Hello."),
                                  .speaker = "Eileen",
                                  .source = "Hello.",
                                  .translation = "Konnichiwa.",
                                  .fuzzy = false});
  CHECK(rows[1].fuzzy);
  CHECK(presenter.line()->text == "Konnichiwa.");

  // An edit shows in the say box at once and is in the file.
  CHECK(editor.edit(rows[0].key, "Yaa.").empty());
  presenter.update(0);
  CHECK(presenter.line()->text == "Yaa.");
  const vnmaki::PoFile saved = vnmaki::parse_po(project.read("translations/ja.po"));
  bool found = false;
  for (const vnmaki::PoEntry& entry : saved.entries) {
    found = found || (entry.context == rows[0].key && entry.str == "Yaa.");
  }
  CHECK(found);

  // An edit of a fuzzy entry takes the flag off, and then the text is used.
  CHECK(editor.edit(rows[1].key, "Kawatta {b}x{/b}.").empty());
  CHECK_FALSE(editor.rows()[1].fuzzy);
  CHECK(translator.translate("start", "Eileen", "Plain {b}bold{/b}.") == "Kawatta {b}x{/b}.");

  // A text whose tags do not parse is refused and nothing changes.
  const std::string before = project.read("translations/ja.po");
  CHECK(editor.edit(rows[0].key, "{b}never closed").find("do not parse") != std::string::npos);
  CHECK(project.read("translations/ja.po") == before);
  CHECK(editor.edit("start/00000000", "x") == "no entry 'start/00000000'");

  // An empty text removes the translation: the source text shows.
  CHECK(editor.edit(rows[0].key, "").empty());
  presenter.update(0);
  CHECK(presenter.line()->text == "Hello.");
  CHECK(editor.rows()[0].translation.empty());
}

TEST_CASE("translation editor: a language with no .po file says why") {
  Project project;
  fs::remove(project.dir / "translations/ja.po");
  vnmaki::Translator translator{project.vfs, vnmaki::load_project_config(project.vfs)};
  vnmaki::ScriptRuntime runtime{project.vfs};
  runtime.load_scripts();
  runtime.start("start");
  vnmaki::TranslationEditor editor{translator, runtime, project.dir};
  REQUIRE(translator.set_language("ja").empty());
  CHECK(editor.rows().empty());
  CHECK(editor.problem().find("translations/ja.po does not exist") != std::string::npos);
}

TEST_CASE("translation editor: an edit keeps a change made to the file after it was read") {
  Project project;
  vnmaki::Translator translator{project.vfs, vnmaki::load_project_config(project.vfs)};
  vnmaki::ScriptRuntime runtime{project.vfs};
  runtime.load_scripts();
  runtime.start("start");
  vnmaki::TranslationEditor editor{translator, runtime, project.dir};
  REQUIRE(translator.set_language("ja").empty());
  const std::vector<TranslationRow> rows = editor.rows();
  REQUIRE(rows.size() == 2);

  // Another program (Poedit, extract-strings) changes another entry of the file.
  vnmaki::PoFile outside = vnmaki::parse_po(project.read("translations/ja.po"));
  for (vnmaki::PoEntry& entry : outside.entries) {
    if (entry.context == rows[1].key) {
      entry.str = "Outside.";
    }
  }
  project.write("translations/ja.po", vnmaki::write_po(outside));

  CHECK(editor.edit(rows[0].key, "Yaa.").empty());
  const vnmaki::PoFile saved = vnmaki::parse_po(project.read("translations/ja.po"));
  for (const vnmaki::PoEntry& entry : saved.entries) {
    if (entry.context == rows[0].key) {
      CHECK(entry.str == "Yaa.");
    }
    if (entry.context == rows[1].key) {
      CHECK(entry.str == "Outside.");
    }
  }
  CHECK(editor.rows()[1].translation == "Outside.");
}
