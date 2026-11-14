// `vnmaki check` (engine/tools/check.hpp): the template is clean, and a few cases that the CLI
// tests (generated projects, one for each code) do not cover.

#include "engine/tools/check.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Finding;
using vnmaki::Severity;

std::vector<Finding> check(const std::string& script) {
  return vnmaki::check_project(vnmaki::test::memory_vfs(
      {{"project.toml", "title = \"T\"\n"}, {"scripts/main.lua", script}}));
}

}  // namespace

TEST_CASE("check: the template project has no finding") {
  const std::filesystem::path directory = std::filesystem::path{VNMAKI_TEMPLATES_DIR} / "default";
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(directory)));
  const std::vector<Finding> findings = vnmaki::check_project(vfs);
  CHECK(vnmaki::findings_to_csv(findings) == "file,line,severity,code,message\n");
}

TEST_CASE("check: a clean script has no finding") {
  CHECK(check("label('start', function() say('A', 'Hi {b}there{/b}') end, { ending = true })")
            .empty());
}

TEST_CASE("check: a finding has its file, line, severity and code") {
  const std::vector<Finding> findings = check(
      "label('start', function()\n"
      "  jump('gone')\n"
      "end)");
  REQUIRE(findings.size() == 1);
  CHECK(findings[0].file == "scripts/main.lua");
  CHECK(findings[0].line == 2);
  CHECK(findings[0].severity == Severity::kError);
  CHECK(findings[0].code == "unknown-label");
  CHECK(vnmaki::has_errors(findings));
}

TEST_CASE("check: warnings alone are not errors") {
  const std::vector<Finding> findings = check("label('start', function() narrate('x') end)");
  REQUIRE(findings.size() == 1);
  CHECK(findings[0].code == "dead-end");
  CHECK_FALSE(vnmaki::has_errors(findings));
}

TEST_CASE("check: a menu branch to an unknown label is found, and a computed one is a warning") {
  const std::vector<Finding> findings = check(
      "label('start', function()\n"
      "  local c = menu { 'a', 'b' }\n"
      "  if c == 1 then jump('gone') else call(store.next) end\n"
      "end, { ending = true })");
  REQUIRE(findings.size() == 2);
  CHECK(findings[0].code == "unknown-label");
  CHECK(findings[1].code == "dynamic-edge");
  CHECK(findings[0].line == 3);
}

TEST_CASE("check: a syntax error does not hide the other findings") {
  const std::vector<Finding> findings = check(
      "label('start', function()\n"
      "  jump('gone')\n"
      "end)\n"
      "x = = 1");
  REQUIRE(findings.size() == 2);
  CHECK(findings[0].code == "unknown-label");
  CHECK(findings[1].code == "lua-syntax");
  CHECK(findings[1].line == 4);
}

TEST_CASE("check: a string that is not a literal gives a warning") {
  const std::vector<Finding> findings = check(
      "screen 'hud' { text { title }, button { 'Ok' }, text { 'Hi' .. name } }\n"  // 1
      "label('start', function()\n"                                                // 2
      "  say('A', line)\n"                                                         // 3
      "  narrate(line .. 'x')\n"                                                   // 4
      "  say('A', 'ok') narrate('ok')\n"                                           // 5
      "  menu { 'Yes', no, 'M' .. x }\n"                                           // 6
      "  menu(options)\n"                                                          // 7
      "end, { ending = true })\n");
  REQUIRE(findings.size() == 7);
  const std::vector<std::pair<std::uint32_t, std::string>> expected{
      {1, "a text of screen 'hud'"},   {1, "a text of screen 'hud'"}, {3, "the text of say"},
      {4, "the text of narrate"},      {6, "a menu option"},          {6, "a menu option"},
      {7, "the table of menu options"}};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    CHECK(findings[i].code == "non-literal-string");
    CHECK(findings[i].severity == Severity::kWarning);
    CHECK(findings[i].line == expected[i].first);
    CHECK(findings[i].message.starts_with(expected[i].second));
  }
  CHECK_FALSE(vnmaki::has_errors(findings));
}

namespace {

std::vector<Finding> check_translations(const std::string& po) {
  return vnmaki::check_project(vnmaki::test::memory_vfs(
      {{"project.toml", "title = \"T\"\n"},
       {"scripts/main.lua", "label('start', function() say(nil, 'Hi') end, { ending = true })"},
       {"translations/ja.po", po},
       {"translations/template.pot", "garbage"}}));
}

}  // namespace

TEST_CASE("check: a translation whose tags do not parse is an error") {
  const std::vector<Finding> findings = check_translations(
      "msgid \"\"\nmsgstr \"\"\n\n"                                     // 1-3
      "msgctxt \"start/1\"\nmsgid \"a\"\nmsgstr \"{b}ok{/b}\"\n\n"      // 4-7
      "msgctxt \"start/2\"\nmsgid \"b\"\nmsgstr \"x {wobble} y\"\n\n"   // 8-11
      "#, fuzzy\nmsgctxt \"start/3\"\nmsgid \"c\"\nmsgstr \"{b}\"\n\n"  // 12-16
      "#~ msgctxt \"start/4\"\n#~ msgid \"d\"\n#~ msgstr \"{b}\"\n\n"   // 17-20
      "msgctxt \"screen/t/5\"\nmsgid \"e\"\nmsgstr \"{{x\"\n\n"         // 21-24
      "msgctxt \"start/6\"\nmsgid \"f\"\nmsgstr \"\"\n");               // 25-28
  REQUIRE(findings.size() == 1);
  CHECK(findings[0].file == "translations/ja.po");
  CHECK(findings[0].line == 9);  // the msgid
  CHECK(findings[0].severity == Severity::kError);
  CHECK(findings[0].code == "bad-translation-tag");
  CHECK(findings[0].message.find("start/2") != std::string::npos);
  CHECK(findings[0].message.find("unknown tag 'wobble'") != std::string::npos);
}

TEST_CASE("check: a .po file that cannot be read is an error with its line") {
  const std::vector<Finding> findings = check_translations("msgid \"a\"\nmsgstr \"b\n");
  REQUIRE(findings.size() == 1);
  CHECK(findings[0].code == "po-syntax");
  CHECK(findings[0].file == "translations/ja.po");
  CHECK(findings[0].line == 2);
  CHECK(findings[0].severity == Severity::kError);
}

TEST_CASE("check: CSV quotes a message with a comma") {
  const std::vector<Finding> findings{{.file = "a,b.lua",
                                       .line = 0,
                                       .severity = Severity::kWarning,
                                       .code = "x",
                                       .message = "say \"hi\""}};
  CHECK(vnmaki::findings_to_csv(findings) ==
        "file,line,severity,code,message\n\"a,b.lua\",,warning,x,\"say \"\"hi\"\"\"\n");
}
