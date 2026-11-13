// Dev-mode import (engine/tools/hot_import.hpp): a batch of the file watcher goes through the
// importers. The project is the one of tools/generators/import_fixtures.py, copied to a temp folder.

#include "engine/tools/hot_import.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/tools/file_watcher.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using Clock = vnmaki::FileWatcher::Clock;

}  // namespace

TEST_CASE("hot import: a changed audio file is imported once, then it is in the cache") {
  const fs::path project = vnmaki::test::unique_temp_path("vnmaki_hot_import_", "project");
  fs::remove_all(project);
  fs::copy(fs::path{VNMAKI_FIXTURES_DIR} / "import" / "project", project,
           fs::copy_options::recursive);
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(project)));
  vnmaki::LogBuffer log;
  vnmaki::HotImport import{vfs, project, log};

  // The batch comes from the watcher, as in the game.
  vnmaki::FileWatcher watcher;
  const Clock::time_point start = Clock::now();
  watcher.notify("audio/tone.wav", start);
  watcher.notify("shaders/tint.frag.hlsl", start);  // HotReload's, not imported here
  watcher.notify(".vnmaki/cache/audio-0", start);
  const std::vector<std::string> batch = watcher.poll(start + vnmaki::kWatchBatchWindow * 2);
  REQUIRE(batch.size() == 3);

  import.apply(batch);
  REQUIRE(log.entries().size() == 1);
  CHECK(log.entries().front().level == vnmaki::LogLevel::kInfo);
  CHECK(log.entries().front().text == "imported audio/tone.wav -> audio/tone.opus");
  CHECK(fs::exists(project / ".vnmaki" / "cache"));

  import.apply(batch);  // from the cache: nothing to say
  CHECK(log.entries().size() == 1);

  std::error_code ignored;
  fs::remove_all(project, ignored);
}

TEST_CASE("hot import: a failed import is an error in the log") {
  const fs::path project = vnmaki::test::unique_temp_path("vnmaki_hot_import_", "broken");
  fs::remove_all(project);
  fs::copy(fs::path{VNMAKI_FIXTURES_DIR} / "import" / "project_broken", project,
           fs::copy_options::recursive);
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(project)));
  vnmaki::LogBuffer log;
  vnmaki::HotImport import{vfs, project, log};

  const std::vector<std::string> batch{"models/missing.gltf"};
  import.apply(batch);
  REQUIRE(log.entries().size() == 2);
  CHECK(log.entries().front().level == vnmaki::LogLevel::kError);
  CHECK(log.entries().front().text == "import: models/missing.gltf: buffer 0 'nothing.bin' not found");

  std::error_code ignored;
  fs::remove_all(project, ignored);
}
