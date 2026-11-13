// `vnmaki check` (engine/tools/check.hpp): the template is clean, and a few cases that the CLI
// tests (generated projects, one for each code) do not cover.

#include "engine/tools/check.hpp"

#include <filesystem>
#include <string>
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

TEST_CASE("check: CSV quotes a message with a comma") {
  const std::vector<Finding> findings{{.file = "a,b.lua",
                                       .line = 0,
                                       .severity = Severity::kWarning,
                                       .code = "x",
                                       .message = "say \"hi\""}};
  CHECK(vnmaki::findings_to_csv(findings) ==
        "file,line,severity,code,message\n\"a,b.lua\",,warning,x,\"say \"\"hi\"\"\"\n");
}
