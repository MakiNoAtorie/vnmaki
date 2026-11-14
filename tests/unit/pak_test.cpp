// `.vnpak` archives: the writer, the mount, the patch packs and the choice of a font subset. The
// fuzz target shares fuzz/pak_properties.hpp.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <doctest/doctest.h>

#include "engine/text/text.hpp"
#include "engine/vfs/pak_mount.hpp"
#include "engine/vfs/pak_writer.hpp"
#include "fuzz/pak_properties.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

using vnmaki::PakError;
using vnmaki::PakMount;
using vnmaki::PakWriter;

using Files = std::vector<std::pair<std::string, std::string>>;

constexpr std::size_t kBigFileSize = 100'000;
constexpr unsigned kRandomPaks = 300;
constexpr std::size_t kRandomSize = 200;

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte b : bytes) {
    text.push_back(static_cast<char>(b));
  }
  return text;
}

std::string pak_text(const Files& files) {
  std::ostringstream stream;
  PakWriter writer{stream};
  for (const auto& [path, content] : files) {
    writer.add(path, bytes_of(content));
  }
  writer.finish();
  return stream.str();
}

std::vector<std::byte> pak_bytes(const Files& files) { return bytes_of(pak_text(files)); }

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream{path, std::ios::binary} << text;
}

struct TempDir {
  std::filesystem::path path;
  explicit TempDir(std::string_view name) : path{vnmaki::test::unique_temp_path("vnpak_", name)} {
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

bool fails_to_open(std::vector<std::byte> bytes) {
  try {
    static_cast<void>(PakMount::from_bytes(std::move(bytes)));
  } catch (const PakError&) {
    return true;
  }
  return false;
}

}  // namespace

TEST_CASE("pak: files read back byte for byte, with folders listed") {
  std::string every_byte;
  for (int i = 0; i < 256; ++i) {
    every_byte.push_back(static_cast<char>(i));
  }
  const Files files{{"scripts/main.lua", "say('hi')\n"},
                    {"scripts/deep/x.lua", "-- x"},
                    {"empty.txt", ""},
                    {"bytes.bin", every_byte},
                    {"big.txt", std::string(kBigFileSize, 'a')}};
  const std::unique_ptr<PakMount> pak = PakMount::from_bytes(pak_bytes(files));

  for (const auto& [path, content] : files) {
    CHECK(pak->is_file(path));
    CHECK(text_of(pak->read_file(path)) == content);
    CHECK(pak->read(path).has_value());
  }
  CHECK(pak->paths() == std::vector<std::string>{"big.txt", "bytes.bin", "empty.txt",
                                                 "scripts/deep/x.lua", "scripts/main.lua"});
  CHECK_FALSE(pak->is_file("scripts"));
  CHECK_FALSE(pak->is_file("Empty.txt"));  // case-sensitive, like every mount
  CHECK_FALSE(pak->read("missing.txt").has_value());
  CHECK_THROWS_AS((void)pak->read_file("missing.txt"), PakError);

  const auto root = pak->list("");
  REQUIRE(root.has_value());
  CHECK(root->size() == 4);  // big.txt, bytes.bin, empty.txt and the folder scripts
  const auto scripts = pak->list("scripts");
  REQUIRE(scripts.has_value());
  CHECK(scripts->size() == 2);  // deep (a folder) and main.lua
  CHECK_FALSE(pak->list("empty.txt").has_value());
  CHECK_FALSE(pak->list("nothing").has_value());
}

TEST_CASE("pak: the data is zstd, so a repetitive file is small") {
  const std::string text = pak_text({{"big.txt", std::string(kBigFileSize, 'a')}});
  CHECK(text.size() < kBigFileSize / 10);
  CHECK(text.starts_with("VNPK"));
  CHECK(text.ends_with("VNPE"));
}

TEST_CASE("pak: a pak on disk works through the Vfs, and patch paks override in name order") {
  const TempDir dir{"patch"};
  write_file(dir.path / "game.vnpak",
             pak_text({{"a.txt", "base a"}, {"b.txt", "base b"}, {"c/d.txt", "base d"}}));
  write_file(dir.path / "patch-2.vnpak", pak_text({{"b.txt", "patch 2 b"}}));
  write_file(dir.path / "patch-1.vnpak",
             pak_text({{"b.txt", "patch 1 b"}, {"e.txt", "patch 1 e"}}));
  write_file(dir.path / "other.vnpak", pak_text({{"a.txt", "not mounted"}}));

  vnmaki::Vfs vfs;
  REQUIRE(vnmaki::mount_game_packs(vfs, dir.path));
  CHECK(text_of(*vfs.read("a.txt")) == "base a");
  CHECK(text_of(*vfs.read("b.txt")) == "patch 2 b");
  CHECK(text_of(*vfs.read("c/d.txt")) == "base d");
  CHECK(text_of(*vfs.read("e.txt")) == "patch 1 e");
  CHECK(vfs.list("").size() == 4);  // a.txt, b.txt, c, e.txt

  vnmaki::Vfs none;
  CHECK_FALSE(vnmaki::mount_game_packs(none, dir.path / "nothing"));
}

TEST_CASE("pak: a bad pak on disk is an error that names the file") {
  const TempDir dir{"bad"};
  write_file(dir.path / "game.vnpak", "not a pak, but long enough to have a header and footer");
  vnmaki::Vfs vfs;
  CHECK_THROWS_AS((void)vnmaki::mount_game_packs(vfs, dir.path), PakError);
  try {
    static_cast<void>(PakMount::open(dir.path / "game.vnpak"));
    FAIL("opened a bad pak");
  } catch (const PakError& error) {
    CHECK(std::string{error.what()}.find("game.vnpak") != std::string::npos);
  }
  CHECK_THROWS_AS((void)PakMount::open(dir.path / "missing.vnpak"), PakError);
}

TEST_CASE("pak: damaged paks are errors, never crashes") {
  const std::vector<std::byte> good = pak_bytes({{"a/b.txt", "hello"}, {"c.txt", "world"}});
  REQUIRE_FALSE(fails_to_open(good));

  for (std::size_t length = 0; length < good.size(); ++length) {
    CHECK(fails_to_open({good.begin(), good.begin() + static_cast<std::ptrdiff_t>(length)}));
  }
  auto bad_magic = good;
  bad_magic[0] = std::byte{'X'};
  CHECK(fails_to_open(bad_magic));
  auto bad_version = good;
  bad_version[4] = std::byte{9};
  CHECK(fails_to_open(bad_version));

  // Every changed byte either fails to open, or opens and fails when the file is read, or (a byte
  // of the index that still makes a valid index) reads the same.
  for (std::size_t i = 0; i < good.size(); ++i) {
    auto copy = good;
    copy[i] ^= std::byte{0x55};
    vnmaki::fuzz::check_pak_read_properties(copy);
  }
}

TEST_CASE("pak: a damaged frame or hash is reported when the file is read") {
  std::vector<std::byte> bytes = pak_bytes({{"a.txt", "some text that is stored"}});
  bytes[vnmaki::kPakHeaderSize + 6] ^= std::byte{0xFF};  // inside the zstd frame
  const std::unique_ptr<PakMount> pak = PakMount::from_bytes(bytes);
  CHECK(pak->is_file("a.txt"));
  CHECK_THROWS_AS((void)pak->read_file("a.txt"), PakError);
  CHECK_FALSE(pak->read("a.txt").has_value());
}

TEST_CASE("pak: the writer refuses bad and clashing paths") {
  std::ostringstream stream;
  PakWriter writer{stream};
  const std::vector<std::byte> data = bytes_of("x");
  CHECK_THROWS_AS(writer.add("", data), PakError);
  CHECK_THROWS_AS(writer.add("../x", data), PakError);
  CHECK_THROWS_AS(writer.add("a\\b", data), PakError);
  CHECK_THROWS_AS(writer.add("a//b", data), PakError);
  writer.add("a/b", data);
  CHECK_THROWS_AS(writer.add("a/b", data), PakError);    // twice
  CHECK_THROWS_AS(writer.add("a", data), PakError);      // a folder
  CHECK_THROWS_AS(writer.add("a/b/c", data), PakError);  // inside a file
  writer.add("a/c", data);
  writer.finish();
  CHECK_THROWS_AS(writer.add("z", data), PakError);
  CHECK(PakMount::from_bytes(bytes_of(stream.str()))->paths().size() == 2);
}

TEST_CASE("pak: the index is sorted whatever the order of adding") {
  const std::unique_ptr<PakMount> pak =
      PakMount::from_bytes(pak_bytes({{"z.txt", "1"}, {"a/b.txt", "2"}, {"a-b.txt", "3"}}));
  CHECK(pak->paths() == std::vector<std::string>{"a-b.txt", "a/b.txt", "z.txt"});
}

TEST_CASE("pak: font_path_for_language picks the subset of a pack when it is there") {
  vnmaki::Vfs vfs;
  vfs.mount(PakMount::from_bytes(pak_bytes({{"fonts/a.en.ttf", "x"},
                                            {"fonts/a.ttf", "y"},
                                            {"fonts/b.ttf", "z"},
                                            {"fonts/c.ja.otf", "w"},
                                            {"fonts/d.ttf", "v"}})));
  CHECK(vnmaki::font_path_for_language(vfs, "fonts/a.ttf", "en") == "fonts/a.en.ttf");
  CHECK(vnmaki::font_path_for_language(vfs, "fonts/a.ttf", "ja") == "fonts/a.ttf");
  CHECK(vnmaki::font_path_for_language(vfs, "fonts/b.ttf", "en") == "fonts/b.ttf");
  CHECK(vnmaki::font_path_for_language(vfs, "fonts/c.OTF", "ja") == "fonts/c.ja.otf");
  CHECK(vnmaki::font_path_for_language(vfs, "fonts/a.ttf", "") == "fonts/a.ttf");
  CHECK(vnmaki::font_path_for_language(vfs, "fonts.d/noextension", "en") == "fonts.d/noextension");
}

TEST_CASE("pak: the fuzz properties hold on samples and on seeded random bytes") {
  vnmaki::fuzz::check_pak_read_properties({});
  vnmaki::fuzz::check_pak_round_trip_properties({});
  std::mt19937 random{1};
  for (unsigned i = 0; i < kRandomPaks; ++i) {
    std::vector<std::byte> bytes(random() % kRandomSize);
    for (std::byte& b : bytes) {
      b = static_cast<std::byte>(random());
    }
    vnmaki::fuzz::check_pak_read_properties(bytes);
    vnmaki::fuzz::check_pak_round_trip_properties(bytes);
  }
}
