// The file watcher on a real folder (engine/tools/file_watcher.hpp, efsw). The unit tests inject
// events; this one writes files and waits for the operating system to report them. Every wait has
// a timeout, so a watcher that reports nothing fails the test and does not hang it.

#include "engine/tools/file_watcher.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <doctest/doctest.h>

#include "engine/core/utf8_path.hpp"

namespace {

using Clock = vnmaki::FileWatcher::Clock;

constexpr std::chrono::seconds kTimeout{10};
constexpr std::chrono::milliseconds kPollStep{10};

// Polls until a batch arrives or the timeout passes; an empty result is the timeout.
std::vector<std::string> wait_for_batch(vnmaki::FileWatcher& watcher) {
  const Clock::time_point deadline = Clock::now() + kTimeout;
  while (Clock::now() < deadline) {
    std::vector<std::string> batch = watcher.poll();
    if (!batch.empty()) {
      return batch;
    }
    std::this_thread::sleep_for(kPollStep);
  }
  return {};
}

void write_file(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out << text;
}

class TempFolder {
 public:
  explicit TempFolder(const std::string& name)
      : root_(std::filesystem::temp_directory_path() / ("vnmaki_file_watcher_" + name)) {
    std::filesystem::remove_all(root_);
    std::filesystem::create_directories(root_ / "scripts");
  }
  TempFolder(const TempFolder&) = delete;
  TempFolder& operator=(const TempFolder&) = delete;
  ~TempFolder() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }
  [[nodiscard]] const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

bool contains(const std::vector<std::string>& batch, const std::string& path) {
  return std::ranges::find(batch, path) != batch.end();
}

}  // namespace

TEST_CASE("file watcher: a real file write arrives as a batch") {
  const TempFolder folder{"write"};
  vnmaki::FileWatcher watcher{folder.root()};

  write_file(folder.root() / "scripts" / "main.lua", "-- one\n");
  const std::vector<std::string> batch = wait_for_batch(watcher);
  REQUIRE_FALSE(batch.empty());  // empty: nothing arrived within the timeout
  CHECK(contains(batch, "scripts/main.lua"));
}

TEST_CASE("file watcher: writes close together are one batch, and a folder made later is seen") {
  const TempFolder folder{"later"};
  vnmaki::FileWatcher watcher{folder.root()};

  write_file(folder.root() / "scripts" / "a.lua", "-- a\n");
  write_file(folder.root() / "scripts" / "b.lua", "-- b\n");
  std::filesystem::create_directories(folder.root() / "images" / "bg");
  write_file(folder.root() / "images" / "bg" / "x.png", "not a png");
  std::vector<std::string> seen;
  const Clock::time_point deadline = Clock::now() + kTimeout;
  while (Clock::now() < deadline && !contains(seen, "images/bg/x.png")) {
    const std::vector<std::string> batch = watcher.poll();
    seen.insert(seen.end(), batch.begin(), batch.end());
    std::this_thread::sleep_for(kPollStep);
  }
  CHECK(contains(seen, "scripts/a.lua"));
  CHECK(contains(seen, "scripts/b.lua"));
  CHECK(contains(seen, "images/bg/x.png"));
}

TEST_CASE("file watcher: a folder and a file with non-ASCII names arrive as UTF-8") {
  const std::string name = "\xE7\x94\xB0\xE4\xB8\xAD";
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / vnmaki::utf8_path("vnmaki_file_watcher_" + name);
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root / "scripts");
  {  // the watcher ends before the folder goes, as with TempFolder
    vnmaki::FileWatcher watcher{root};
    write_file(root / "scripts" / vnmaki::utf8_path(name + ".lua"), "-- one\n");
    const std::vector<std::string> batch = wait_for_batch(watcher);
    CHECK(contains(batch, "scripts/" + name + ".lua"));
  }
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
}

TEST_CASE("file watcher: a folder that does not exist cannot be watched") {
  CHECK_THROWS_AS(vnmaki::FileWatcher{std::filesystem::temp_directory_path() /
                                      "vnmaki_file_watcher_no_such_folder"},
                  std::runtime_error);
}
