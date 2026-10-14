// project.toml parsing (docs/SPEC.md section 19).

#include "engine/runtime/project_config.hpp"

#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::ProjectConfig;
using vnmaki::ProjectConfigError;
using vnmaki::Resolution;
using Strings = std::vector<std::string>;

// The message of the ProjectConfigError parsing `text` throws.
std::string error_for(const std::string& text) {
  std::string message;
  try {
    static_cast<void>(vnmaki::parse_project_config(text));
  } catch (const ProjectConfigError& error) {
    message = error.what();
  }
  CHECK_MESSAGE(!message.empty(), "no ProjectConfigError for: " << text);
  return message;
}

}  // namespace

TEST_CASE("project.toml: reads title, version, resolution and languages") {
  const ProjectConfig config = vnmaki::parse_project_config(R"(
title = "The Long Rain"
version = "1.2.0"
resolution = [1280, 720]
languages = ["en", "ja"]
)");

  CHECK(config.title == "The Long Rain");
  CHECK(config.version == "1.2.0");
  CHECK(config.resolution == Resolution{.width = 1280, .height = 720});
  CHECK(config.languages == Strings{"en", "ja"});
}

TEST_CASE("project.toml: only the title is required") {
  const ProjectConfig config = vnmaki::parse_project_config("title = \"Short\"\n");

  CHECK(config.title == "Short");
  CHECK(config.version == "0.0.0");
  CHECK(config.resolution == Resolution{.width = 1920, .height = 1080});
  CHECK(config.languages == Strings{"en"});
}

TEST_CASE("project.toml: keys for later features are ignored") {
  const ProjectConfig config = vnmaki::parse_project_config(R"(
title = "Game"
platforms = ["windows"]
[build]
compress = true
)");

  CHECK(config.title == "Game");
}

TEST_CASE("project.toml: invalid files are reported with the place and the reason") {
  CHECK(error_for("version = \"1.0\"\n") == "project.toml: missing required key 'title'");
  CHECK(error_for("title = 3\n") == "project.toml:1:9: 'title' must be a string");
  CHECK(error_for("title = \"\"\n") == "project.toml:1:9: 'title' must not be empty");
  CHECK(error_for("title = \"A\"\nversion = 1\n") ==
        "project.toml:2:11: 'version' must be a string");
  CHECK(error_for("title = \"A\"\nresolution = [1280]\n") ==
        "project.toml:2:14: 'resolution' must be [width, height] with whole numbers from 1 to "
        "16384");
  CHECK(error_for("title = \"A\"\nresolution = [0, 720]\n") ==
        "project.toml:2:14: 'resolution' must be [width, height] with whole numbers from 1 to "
        "16384");
  CHECK(error_for("title = \"A\"\nresolution = \"1280x720\"\n") ==
        "project.toml:2:14: 'resolution' must be [width, height] with whole numbers from 1 to "
        "16384");
  CHECK(error_for("title = \"A\"\nlanguages = []\n") ==
        "project.toml:2:13: 'languages' must be a non-empty list of language codes");
  CHECK(error_for("title = \"A\"\nlanguages = [\"en\", 2]\n") ==
        "project.toml:2:13: 'languages' must be a non-empty list of language codes");
  CHECK(error_for("title = \"A\"\nlanguages = [\"en\", \"en\"]\n") ==
        "project.toml:2:13: 'languages' lists 'en' twice");
}

TEST_CASE("project.toml: each language has a font fallback chain") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"project.toml", R"(
title = "Game"
languages = ["en", "ja", "he"]

[fonts.fallback]
en = ["fonts/latin.ttf"]
ja = ["fonts/japanese.otf", "fonts/latin.ttf"]
)"}});
  const ProjectConfig config = vnmaki::load_project_config(vfs);

  REQUIRE(config.font_fallbacks.size() == 2);
  CHECK(config.font_fallbacks.at("en") == Strings{"fonts/latin.ttf"});
  CHECK(config.font_fallbacks.at("ja") == Strings{"fonts/japanese.otf", "fonts/latin.ttf"});
  // A language without a chain has none.
  CHECK(config.font_fallbacks.count("he") == 0);
  CHECK(vnmaki::parse_project_config("title = \"A\"\n").font_fallbacks.empty());
}

TEST_CASE("project.toml: a bad font fallback chain is reported with the place") {
  CHECK(error_for("title = \"A\"\nfonts = 3\n") == "project.toml:2:9: 'fonts' must be a table");
  CHECK(error_for("title = \"A\"\n[fonts]\nfallback = \"a.ttf\"\n") ==
        "project.toml:3:12: 'fonts.fallback' must be a table of font path lists, one for each "
        "language");
  CHECK(error_for("title = \"A\"\n[fonts.fallback]\nen = \"a.ttf\"\n") ==
        "project.toml:3:6: 'fonts.fallback.en' must be a list of font paths");
  CHECK(error_for("title = \"A\"\n[fonts.fallback]\nen = [\"a.ttf\", 3]\n") ==
        "project.toml:3:6: 'fonts.fallback.en' must be a list of font paths");
  CHECK(error_for("title = \"A\"\n[fonts.fallback]\nen = [\"\"]\n") ==
        "project.toml:3:6: 'fonts.fallback.en' must be a list of font paths");
  CHECK(error_for("title = \"A\"\n[fonts.fallback]\nja = [\"a.ttf\"]\n") ==
        "project.toml:3:6: 'fonts.fallback' has language 'ja', which 'languages' does not list");
}

TEST_CASE("project.toml: TOML syntax errors give the line and column") {
  const std::string message = error_for("title = \"A\"\nversion = \n");

  CHECK(message.starts_with("project.toml:2:"));
}

TEST_CASE("project.toml: is loaded from the root of the vfs") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"project.toml", "title = \"From VFS\"\n"}});

  CHECK(vnmaki::load_project_config(vfs).title == "From VFS");
}

TEST_CASE("project.toml: a missing file is an error") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", ""}});

  CHECK_THROWS_WITH_AS(static_cast<void>(vnmaki::load_project_config(vfs)),
                       "project.toml: not found", ProjectConfigError);
}
