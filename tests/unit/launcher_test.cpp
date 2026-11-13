// The launcher without ImGui (cli/launcher.hpp): the recent-project list, and the actions on
// temporary folders. Run starts a process and Open folder opens a window, so those two use fakes.

#include "cli/launcher.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "engine/core/utf8_path.hpp"
#include "engine/platform/shell.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using vnmaki::cli::Launcher;
using vnmaki::cli::LauncherAction;
using vnmaki::cli::LauncherHost;
using vnmaki::cli::RecentProjects;

// A directory under the system temp directory, empty at the start and removed at the end.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_launcher_test_", name)) {
    fs::remove_all(path_);
    fs::create_directories(path_);
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

void write_file(const fs::path& path, std::string_view content) {
  fs::create_directories(path.parent_path());
  std::ofstream{path, std::ios::binary | std::ios::trunc} << content;
}

// A project of one line, with a second language so that Extract strings has a file to update.
void write_project(const fs::path& folder) {
  write_file(folder / "project.toml", "title = \"T\"\nlanguages = [\"en\", \"ja\"]\n");
  write_file(folder / "scripts" / "main.lua",
             "label('start', function() say('A', 'Hello') end, { ending = true })\n");
}

struct FakeHost {
  std::vector<std::vector<fs::path>> spawned;
  std::vector<fs::path> opened;
  bool works = true;

  LauncherHost host(std::optional<fs::path> runner = std::nullopt) {
    return {.spawn_vnmaki =
                [this](std::span<const fs::path> args) {
                  spawned.emplace_back(args.begin(), args.end());
                  return works;
                },
            .open_folder =
                [this](const fs::path& folder) {
                  opened.push_back(folder);
                  return works;
                },
            .runner = std::move(runner)};
  }
};

bool contains(const std::string& text, std::string_view part) {
  return text.find(part) != std::string::npos;
}

}  // namespace

TEST_CASE("recent projects: newest first, no duplicates, at most ten, kept in the file") {
  const TempDir temp{"recent"};
  const fs::path file = temp.path() / "data" / "recent.txt";  // the folder does not exist yet
  RecentProjects recent{file};
  CHECK(recent.list().empty());

  CHECK(recent.add(temp.path() / "a"));
  CHECK(recent.add(temp.path() / "b"));
  CHECK(recent.add(temp.path() / "a" / ".." / "a"));  // the first folder, spelled another way
  REQUIRE(recent.list().size() == 2);
  CHECK(recent.list()[0] == (temp.path() / "a"));
  CHECK(recent.list()[1] == (temp.path() / "b"));

  const RecentProjects reloaded{file};
  CHECK(reloaded.list() == recent.list());

  for (std::size_t i = 0; i < RecentProjects::kMaxProjects + 3; ++i) {
    CHECK(recent.add(temp.path() / ("p" + std::to_string(i))));
  }
  REQUIRE(recent.list().size() == RecentProjects::kMaxProjects);
  CHECK(recent.list().front() ==
        (temp.path() / ("p" + std::to_string(RecentProjects::kMaxProjects + 2))));
  CHECK(RecentProjects{file}.list() == recent.list());
}

TEST_CASE("recent projects: a trailing separator adds no folder; a bad file is reported") {
  const TempDir temp{"recent_edge"};
  RecentProjects recent{temp.path() / "recent.txt"};
  CHECK(recent.add(temp.path() / "game"));
  CHECK(recent.add(temp.path() / "game" / ""));
  CHECK(recent.list().size() == 1);

  // The file is a folder, so it cannot be written; the list in memory still changes.
  fs::create_directories(temp.path() / "blocked");
  RecentProjects blocked{temp.path() / "blocked"};
  CHECK_FALSE(blocked.add(temp.path() / "game"));
  CHECK(blocked.list().size() == 1);
}

TEST_CASE("launcher: New, Check, Extract strings and Build windows on a temporary folder") {
  const TempDir temp{"actions"};
  FakeHost fake;
  const fs::path runner = temp.path() / "runner.exe";
  write_file(runner, "not a program; the build only copies it");
  Launcher launcher{RecentProjects{temp.path() / "data" / "recent.txt"}, fake.host(runner)};

  SUBCASE("New makes the template and remembers it; a second New on it fails") {
    const fs::path game = temp.path() / "made";
    CHECK(launcher.run(LauncherAction::kNew, game.string()) == 0);
    CHECK(fs::is_regular_file(game / "project.toml"));
    CHECK(contains(launcher.log(), "> vnmaki new"));
    REQUIRE(launcher.recent().list().size() == 1);
    CHECK(launcher.recent().list().front() == game);

    CHECK(launcher.run(LauncherAction::kNew, game.string()) == 1);
    CHECK(launcher.recent().list().size() == 1);
  }

  SUBCASE("Check prints the findings of a good project and a bad folder fails") {
    const fs::path game = temp.path() / "game";
    write_project(game);
    CHECK(launcher.run(LauncherAction::kCheck, game.string()) == 0);
    CHECK(contains(launcher.log(), "> vnmaki check"));
    CHECK(launcher.recent().list().size() == 1);

    launcher.clear_log();
    CHECK(launcher.run(LauncherAction::kCheck, (temp.path() / "nothing").string()) == 1);
    CHECK_FALSE(launcher.log().empty());
    CHECK(launcher.recent().list().size() == 1);  // the bad folder is not remembered
  }

  SUBCASE("Extract strings writes the translation files") {
    const fs::path game = temp.path() / "game";
    write_project(game);
    CHECK(launcher.run(LauncherAction::kExtractStrings, game.string()) == 0);
    CHECK(fs::is_regular_file(game / "translations" / "template.pot"));
    CHECK(fs::is_regular_file(game / "translations" / "ja.po"));
  }

  SUBCASE("Build windows writes the game to .vnmaki/windows with the given runner") {
    const fs::path game = temp.path() / "game";
    write_project(game);
    CHECK(launcher.run(LauncherAction::kBuildWindows, game.string()) == 0);
    CHECK(fs::is_regular_file(game / ".vnmaki" / "windows" / "game.vnpak"));
    CHECK(fs::is_regular_file(game / ".vnmaki" / "windows" / "T.exe"));
    CHECK(contains(launcher.log(), "packed"));
  }

  SUBCASE("Build windows fails without a runner and says so") {
    const fs::path game = temp.path() / "game";
    write_project(game);
    Launcher no_runner{RecentProjects{temp.path() / "recent2.txt"}, fake.host()};
    CHECK(no_runner.run(LauncherAction::kBuildWindows, game.string()) == 1);
    CHECK_FALSE(no_runner.log().empty());
    CHECK(no_runner.recent().list().empty());
  }
}

TEST_CASE("launcher: a folder with a non-ASCII name works as UTF-8 and is kept as UTF-8") {
  const TempDir temp{"utf8"};
  FakeHost fake;
  const fs::path runner = temp.path() / "runner.exe";
  write_file(runner, "not a program; the build only copies it");
  const fs::path data = temp.path() / vnmaki::utf8_path("\xE3\x83\x87\xE3\x83\xBC\xE3\x82\xBF");
  const std::string name = "\xE7\x94\xB0\xE4\xB8\xAD";  // a name outside the ANSI code page
  const fs::path game = temp.path() / vnmaki::utf8_path(name);
  Launcher launcher{RecentProjects{data / "recent.txt"}, fake.host(runner)};
  const std::string folder = vnmaki::to_utf8(game);  // what the ImGui text field holds

  REQUIRE(launcher.run(LauncherAction::kNew, folder) == 0);
  CHECK_MESSAGE(fs::is_regular_file(game / "project.toml"), launcher.log());
  CHECK(launcher.run(LauncherAction::kCheck, folder) == 0);
  fs::remove_all(game / "shaders");  // a release build has no shader compiler to import them
  CHECK(launcher.run(LauncherAction::kBuildWindows, folder) == 0);
  CHECK(fs::is_regular_file(game / ".vnmaki" / "windows" / "game.vnpak"));
  CHECK(launcher.run(LauncherAction::kRun, folder) == 0);
  REQUIRE(fake.spawned.size() == 1);
  CHECK(fake.spawned.front().at(1) == game);
  CHECK(launcher.run(LauncherAction::kOpenFolder, folder) == 0);
  CHECK(fake.opened.back() == game);

  // The recent list holds the folder, and recent.txt holds its UTF-8 bytes.
  REQUIRE(launcher.recent().list().size() == 1);
  CHECK(launcher.recent().list().front() == game);
  CHECK(RecentProjects{data / "recent.txt"}.list() == launcher.recent().list());
  std::ifstream file{data / "recent.txt", std::ios::binary};
  const std::string text{std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
  CHECK(text.find(name) != std::string::npos);
}

TEST_CASE("launcher: New, Check and Build windows on one temporary folder") {
  const TempDir temp{"demo"};
  FakeHost fake;
  const fs::path runner = temp.path() / "runner.exe";
  write_file(runner, "not a program; the build only copies it");
  Launcher launcher{RecentProjects{temp.path() / "data" / "recent.txt"}, fake.host(runner)};
  const fs::path game = temp.path() / "game";

  // New makes the template, with its quick menu and its Japanese sample.
  REQUIRE(launcher.run(LauncherAction::kNew, game.string()) == 0);
  CHECK(fs::is_regular_file(game / "scripts" / "ui.lua"));
  CHECK(fs::is_regular_file(game / "translations" / "ja.po"));

  // Check finds nothing in the template: the log has the command and the CSV header only.
  launcher.clear_log();
  CHECK(launcher.run(LauncherAction::kCheck, game.string()) == 0);
  const std::string command = "> vnmaki check \"" + game.string() + "\"";
  CHECK(launcher.log() == command + "\nfile,line,severity,code,message\n");

  // Build windows packs the folder. A build with no shader compiler cannot import the sepia
  // shader, so it gets the precompiled file in its place (as tests/cli/run_build.cmake does).
  if (!vnmaki::shader_compiler_available()) {
    const fs::path shaders = game / "shaders";
    fs::copy_file(fs::path{VNMAKI_PRECOMPILED_DIR} / "shaders" / "sepia.frag.vnshader",
                  shaders / "sepia.frag.vnshader");
    fs::remove(shaders / "sepia.frag.hlsl");
  }
  launcher.clear_log();
  REQUIRE(launcher.run(LauncherAction::kBuildWindows, game.string()) == 0);
  CHECK(contains(launcher.log(), "packed"));
  const fs::path out = game / ".vnmaki" / "windows";
  CHECK(fs::is_regular_file(out / "game.exe"));
  CHECK(fs::is_regular_file(out / "game.vnpak"));
  const std::unique_ptr<vnmaki::PakMount> pak = vnmaki::PakMount::open(out / "game.vnpak");
  REQUIRE(pak != nullptr);
  for (const char* path : {"project.toml", "scripts/main.lua", "scripts/ui.lua",
                           "translations/ja.po", "images/bg/classroom.png"}) {
    CHECK_MESSAGE(pak->is_file(path), path);
  }
  CHECK_FALSE(pak->is_file(".vnmaki/windows/game.vnpak"));  // the output is not packed into itself
  // The folder is in the recent list once, newest first.
  REQUIRE(launcher.recent().list().size() == 1);
  CHECK(launcher.recent().list().front() == game);
}

TEST_CASE("launcher: Run starts `vnmaki run <folder>` as a process; Open folder opens a folder") {
  const TempDir temp{"process"};
  FakeHost fake;
  Launcher launcher{RecentProjects{temp.path() / "recent.txt"}, fake.host()};
  const fs::path game = temp.path() / "game";

  CHECK(launcher.run(LauncherAction::kRun, game.string()) == 0);
  REQUIRE(fake.spawned.size() == 1);
  CHECK(fake.spawned[0] == std::vector<fs::path>{"run", game});
  CHECK(launcher.recent().list().size() == 1);

  CHECK(launcher.run(LauncherAction::kOpenFolder, game.string()) == 0);
  REQUIRE(fake.opened.size() == 1);
  CHECK(fake.opened[0] == game);

  fake.works = false;
  const fs::path other = temp.path() / "other";
  CHECK(launcher.run(LauncherAction::kRun, other.string()) == 1);
  CHECK(launcher.run(LauncherAction::kOpenFolder, other.string()) == 1);
  CHECK(launcher.recent().list().size() == 1);  // a failure is not remembered
}

TEST_CASE("launcher: an empty folder is a usage error and runs nothing") {
  const TempDir temp{"empty"};
  FakeHost fake;
  Launcher launcher{RecentProjects{temp.path() / "recent.txt"}, fake.host()};
  for (const LauncherAction action :
       {LauncherAction::kNew, LauncherAction::kRun, LauncherAction::kCheck,
        LauncherAction::kExtractStrings, LauncherAction::kBuildWindows,
        LauncherAction::kOpenFolder}) {
    CHECK(launcher.run(action, "") == 2);
  }
  CHECK(fake.spawned.empty());
  CHECK(fake.opened.empty());
  CHECK(launcher.recent().list().empty());
}

#ifdef _WIN32
TEST_CASE("shell: spawn_process starts a program and reports one that does not exist") {
  const TempDir temp{"spawn"};
  const fs::path self = vnmaki::executable_path();
  REQUIRE(fs::is_regular_file(self));
  // The child lists the (no) test suites and ends; its exit is not waited for.
  const std::vector<fs::path> args{"--list-test-suites", "a \"quoted\" arg\\", "with space"};
  CHECK(vnmaki::spawn_process(self, args));
  CHECK_FALSE(vnmaki::spawn_process(temp.path() / "no_such_program.exe", {}));
}
#endif
