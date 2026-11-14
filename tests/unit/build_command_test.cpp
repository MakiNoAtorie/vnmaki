// `vnmaki build windows` and `vnmaki run` on temporary folders: what the pack takes, and which
// source `run` chooses when a folder has both loose files and a pack.

#include "cli/build_command.hpp"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "cli/run_command.hpp"
#include "engine/core/utf8_path.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/pak_writer.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;

class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_build_test_", name)) {
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ignored;
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

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

std::string script(std::string_view line) {
  return "label('start', function() say('A', '" + std::string{line} +
         "') end, { ending = true })\n";
}

void write_project(const fs::path& folder, std::string_view line) {
  write_file(folder / "project.toml", "title = \"T\"\n");
  write_file(folder / "scripts" / "main.lua", script(line));
}

// A pak with the same project, whose line is `line`.
void write_pak(const fs::path& file, std::string_view line) {
  std::ostringstream stream;
  {
    vnmaki::PakWriter writer{stream};
    writer.add("project.toml", bytes_of("title = \"T\"\n"));
    writer.add("scripts/main.lua", bytes_of(script(line)));
    writer.finish();
  }
  write_file(file, stream.str());
}

int build(const fs::path& project, const fs::path& out, const fs::path& runner,
          std::ostringstream& err) {
  const std::string project_text = vnmaki::to_utf8(project);
  const std::string out_text = vnmaki::to_utf8(out);
  const std::string runner_text = vnmaki::to_utf8(runner);
  const std::vector<std::string_view> args{"windows", project_text, "--out",
                                           out_text,  "--runner",   runner_text};
  std::ostringstream out_stream;
  return vnmaki::cli::build_command(args, out_stream, err);
}

std::vector<std::string> pak_paths(const fs::path& pak) {
  return vnmaki::PakMount::open(pak)->paths();
}

bool has(const std::vector<std::string>& paths, std::string_view path) {
  return std::ranges::find(paths, path) != paths.end();
}

}  // namespace

TEST_CASE("build: --out as the project folder is refused") {
  const TempDir temp{"same"};
  const fs::path game = temp.path() / "game";
  write_project(game, "Hello");
  write_file(temp.path() / "runner.exe", "runner");

  std::ostringstream err;
  CHECK(build(game, game, temp.path() / "runner.exe", err) == 1);
  CHECK(err.str().find("--out must not be the project folder") != std::string::npos);
  CHECK(build(game, game / "." / "", temp.path() / "runner.exe", err) == 1);
  CHECK_FALSE(fs::exists(game / "game.vnpak"));
}

TEST_CASE("build: the pack leaves out what an earlier build wrote at the top of the project") {
  const TempDir temp{"stale"};
  const fs::path game = temp.path() / "game";
  write_project(game, "Hello");
  write_file(game / "game.vnpak", "stale pack");
  write_file(game / "patch-1.vnpak", "stale patch");
  write_file(game / "Old.EXE", "stale exe");
  write_file(game / "SDL3.dll", "stale dll");
  write_file(game / "assets" / "tool.exe", "kept: not at the top");
  write_file(game / "readme.txt", "kept");
  write_file(temp.path() / "runner.exe", "runner");

  std::ostringstream err;
  REQUIRE_MESSAGE(build(game, temp.path() / "out", temp.path() / "runner.exe", err) == 0,
                  err.str());
  const std::vector<std::string> paths = pak_paths(temp.path() / "out" / "game.vnpak");
  CHECK(has(paths, "project.toml"));
  CHECK(has(paths, "readme.txt"));
  CHECK(has(paths, "assets/tool.exe"));
  CHECK_FALSE(has(paths, "game.vnpak"));
  CHECK_FALSE(has(paths, "patch-1.vnpak"));
  CHECK_FALSE(has(paths, "Old.EXE"));
  CHECK_FALSE(has(paths, "SDL3.dll"));
}

TEST_CASE("run: a folder with project.toml plays its loose files, not a stale game.vnpak") {
  const TempDir temp{"run"};
  const fs::path game = temp.path() / "game";
  write_project(game, "Loose line");
  write_pak(game / "game.vnpak", "Stale line");
  const std::string game_text = vnmaki::to_utf8(game);
  const std::string data_text = vnmaki::to_utf8(temp.path() / "data");
  const std::vector<std::string_view> args{game_text, "--headless", "--data-dir", data_text};

  std::ostringstream out;
  std::ostringstream err;
  CHECK(vnmaki::cli::run_command(args, out, err) == 0);
  CHECK(out.str().find("Loose line") != std::string::npos);
  CHECK(out.str().find("Stale line") == std::string::npos);
  CHECK(err.str().find("game.vnpak") != std::string::npos);  // it says the pack is ignored

  // Without project.toml on disk the folder is a built game: the pack is the whole project.
  fs::remove(game / "project.toml");
  fs::remove_all(game / "scripts");
  std::ostringstream packed_out;
  std::ostringstream packed_err;
  CHECK(vnmaki::cli::run_command(args, packed_out, packed_err) == 0);
  CHECK(packed_out.str().find("Stale line") != std::string::npos);
  CHECK(packed_err.str().empty());
}
