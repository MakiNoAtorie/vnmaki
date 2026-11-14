// Fixtures come from tools/generators/vfs_fixtures.py; the expected contents
// below are the ones that script writes.

#include "engine/vfs/vfs.hpp"

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

namespace {

const std::filesystem::path kFixtures = std::filesystem::path{VNMAKI_FIXTURES_DIR} / "vfs";

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

std::optional<std::vector<std::byte>> contents(std::string_view text) { return bytes_of(text); }

vnmaki::Vfs vfs_with(std::initializer_list<std::string_view> mount_dirs) {
  vnmaki::Vfs vfs;
  for (const std::string_view dir : mount_dirs) {
    REQUIRE(vfs.mount(vnmaki::make_directory_mount(kFixtures / dir)));
  }
  return vfs;
}

}  // namespace

TEST_CASE("vfs: reads a whole file from a loose-file mount") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK(vfs.read("hello.txt") == contents("hello from base\n"));
  CHECK(vfs.read("dir/sub/b.txt") == contents("b from base\n"));
}

TEST_CASE("vfs: reads binary and empty files byte for byte") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  constexpr int kByteValues = 256;
  std::vector<std::byte> every_byte_value;
  every_byte_value.reserve(kByteValues);
  for (int value = 0; value < kByteValues; ++value) {
    every_byte_value.push_back(static_cast<std::byte>(value));
  }
  CHECK(vfs.read("bytes.bin") == std::optional{every_byte_value});
  CHECK(vfs.read("empty.bin") == std::optional{std::vector<std::byte>{}});
}

TEST_CASE("vfs: virtual paths are normalized before lookup") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK(vfs.read("dir\\sub\\..\\.\\a.txt") == contents("a from base\n"));
  CHECK(vfs.read("dir//sub/b.txt") == contents("b from base\n"));
}

TEST_CASE("vfs: read finds nothing for missing files, directories and the root") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK_FALSE(vfs.read("missing.txt").has_value());
  CHECK_FALSE(vfs.read("dir/missing/a.txt").has_value());
  CHECK_FALSE(vfs.read("hello.txt/child").has_value());
  CHECK_FALSE(vfs.read("dir").has_value());
  CHECK_FALSE(vfs.read("").has_value());
}

TEST_CASE("vfs: files outside the mount root cannot be reached") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK_FALSE(vfs.read("../outside.txt").has_value());
  CHECK_FALSE(vfs.read("dir/../../outside.txt").has_value());
  CHECK_FALSE(vfs.read((kFixtures / "outside.txt").generic_string()).has_value());
}

TEST_CASE("vfs: an empty VFS has no files") {
  const vnmaki::Vfs vfs;

  CHECK_FALSE(vfs.read("hello.txt").has_value());
  CHECK_FALSE(vfs.exists("hello.txt"));
}

TEST_CASE("vfs: exists is true for files only") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK(vfs.exists("hello.txt"));
  CHECK(vfs.exists("empty.bin"));
  CHECK(vfs.exists("dir/sub/b.txt"));
  CHECK(vfs.exists("./dir\\a.txt"));
  CHECK_FALSE(vfs.exists("missing.txt"));
  CHECK_FALSE(vfs.exists("dir"));
  CHECK_FALSE(vfs.exists(""));
  CHECK_FALSE(vfs.exists("../outside.txt"));
}

// Matters on case-insensitive host file systems (Windows, macOS): the
// directory mount must still match every segment exactly.
TEST_CASE("vfs: paths are case-sensitive on every platform") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK(vfs.read("Case.txt") == contents("exact case\n"));
  CHECK_FALSE(vfs.read("case.txt").has_value());
  CHECK_FALSE(vfs.exists("CASE.TXT"));
  CHECK_FALSE(vfs.exists("DIR/a.txt"));
  CHECK_FALSE(vfs.exists("dir/A.txt"));
  CHECK(vfs.list("Dir").empty());
}

TEST_CASE("vfs: a directory mount needs an existing directory") {
  CHECK(vnmaki::make_directory_mount(kFixtures / "missing") == nullptr);
  CHECK(vnmaki::make_directory_mount(kFixtures / "outside.txt") == nullptr);

  vnmaki::Vfs vfs;
  CHECK_FALSE(vfs.mount(nullptr));
}

TEST_CASE("vfs: a later mount overrides an earlier one by path") {
  const vnmaki::Vfs vfs = vfs_with({"base", "patch"});

  CHECK(vfs.read("override.txt") == contents("patch version\n"));
  CHECK(vfs.read("dir/shared.txt") == contents("shared from patch\n"));
  CHECK(vfs.read("hello.txt") == contents("hello from base\n"));
  CHECK(vfs.read("dir/c.txt") == contents("c from patch\n"));
  CHECK(vfs.exists("dir/sub/b.txt"));
}

TEST_CASE("vfs: mount order decides which file wins") {
  const vnmaki::Vfs vfs = vfs_with({"patch", "base"});

  CHECK(vfs.read("override.txt") == contents("base version\n"));
  CHECK(vfs.read("dir/shared.txt") == contents("shared from base\n"));
  CHECK(vfs.read("dir/c.txt") == contents("c from patch\n"));
}

TEST_CASE("vfs: lists a directory's files and subdirectories sorted by name") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK(vfs.list("") == std::vector<vnmaki::VfsEntry>{
                            {.name = "Case.txt", .is_directory = false},
                            {.name = "bytes.bin", .is_directory = false},
                            {.name = "dir", .is_directory = true},
                            {.name = "empty.bin", .is_directory = false},
                            {.name = "hello.txt", .is_directory = false},
                            {.name = "override.txt", .is_directory = false},
                        });
  CHECK(vfs.list("dir/sub") == std::vector<vnmaki::VfsEntry>{
                                   {.name = "b.txt", .is_directory = false},
                               });
}

TEST_CASE("vfs: listing merges every mount, each name once") {
  const vnmaki::Vfs vfs = vfs_with({"base", "patch"});

  // base/dir also holds "invalid?.txt" (except on Windows, which cannot
  // create it). No virtual path can name it, so it is not listed.
  CHECK(vfs.list("./dir/") == std::vector<vnmaki::VfsEntry>{
                                  {.name = "a.txt", .is_directory = false},
                                  {.name = "c.txt", .is_directory = false},
                                  {.name = "shared.txt", .is_directory = false},
                                  {.name = "sub", .is_directory = true},
                              });
}

TEST_CASE("vfs: listing a missing directory, a file or an invalid path is empty") {
  const vnmaki::Vfs vfs = vfs_with({"base"});

  CHECK(vfs.list("missing").empty());
  CHECK(vfs.list("hello.txt").empty());
  CHECK(vfs.list("..").empty());
  CHECK(vfs.list(kFixtures.generic_string()).empty());
}
