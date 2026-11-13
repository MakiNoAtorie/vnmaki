// UTF-8 text and host paths (engine/core/utf8_path.hpp): a name outside the ANSI code page must
// reach the file system as it is.

#include "engine/core/utf8_path.hpp"

#include <filesystem>
#include <fstream>
#include <string>

#include <doctest/doctest.h>

#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;

// "tanaka" in kanji, as UTF-8 bytes.
constexpr const char* kName = "\xE7\x94\xB0\xE4\xB8\xAD";

}  // namespace

TEST_CASE("utf8_path: a name outside the ANSI code page is made and found on disk") {
  const fs::path folder = vnmaki::test::unique_temp_path("vnmaki_utf8_", "dir") /
                          vnmaki::utf8_path(kName);
  fs::remove_all(folder);
  fs::create_directories(folder);
  std::ofstream{folder / vnmaki::utf8_path(std::string{kName} + ".txt")} << "x";

  CHECK(fs::is_regular_file(folder / vnmaki::utf8_path(std::string{kName} + ".txt")));
  CHECK(vnmaki::to_utf8(folder.filename()) == kName);
  CHECK(vnmaki::to_utf8(folder / "a") ==
        vnmaki::to_utf8(folder) + static_cast<char>(fs::path::preferred_separator) + "a");
  std::error_code ignored;
  fs::remove_all(folder.parent_path(), ignored);
}
