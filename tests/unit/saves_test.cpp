// Saves (docs/SPEC.md section 8): the file format, thumbnails, slots, quick save, auto save, load
// through replay, the on_load hook and the script functions. Expected values are written out by
// hand from the script text.

#include "engine/saves/saves.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/render/image.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/save_file.hpp"
#include "engine/saves/thumbnail.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::AutoSaveEvent;
using vnmaki::MenuChoice;
using vnmaki::NarrateRequest;
using vnmaki::RunState;
using vnmaki::SaveData;
using vnmaki::SaveError;
using vnmaki::SavesOptions;
using vnmaki::ScreenResult;
using vnmaki::ScreenValue;
using vnmaki::ScriptPosition;
using vnmaki::StoreSnapshot;
using vnmaki::StoreTable;
using vnmaki::StoreTableRef;
using vnmaki::StoreValue;
using vnmaki::Thumbnail;
using Bytes = std::vector<std::byte>;

constexpr std::int64_t kClockStart = 1'700'000'000;
constexpr std::uint64_t kSomeHash = 0xFEEDFACECAFEBEEFULL;
constexpr double kSomeNumber = 0.25;
constexpr std::int64_t kSomeInteger = -123456789;
constexpr std::size_t kChannels = 4;
constexpr std::uint64_t kSeed = 12345;
constexpr int kRandomInputs = 300;
constexpr std::uint64_t kMaxRandomLength = 400;
constexpr std::size_t kGarbageSize = 20;
constexpr std::byte kGarbageByte{7};
constexpr std::uint8_t kByteFlip = 0xFF;
constexpr std::uint8_t kFixMap14 = 0x8E;

// A directory under the system temp directory, empty at the start and removed at the end.
class TempDir {
 public:
  explicit TempDir(const std::string& name)
      : path_(std::filesystem::temp_directory_path() / ("vnmaki_saves_test_" + name)) {
    std::filesystem::remove_all(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir(TempDir&&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  TempDir& operator=(TempDir&&) = delete;
  ~TempDir() {
    try {
      std::filesystem::remove_all(path_);
    } catch (...) {
      // A directory left in the temp directory is not worth failing a test for.
      std::fputs("TempDir: cannot remove the directory\n", stderr);
    }
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

// A small deterministic generator (splitmix64): the same inputs on every platform.
class Random {
 public:
  explicit Random(std::uint64_t seed) : state_(seed) {}

  // A number from 0 to `limit` - 1.
  std::uint64_t below(std::uint64_t limit) {
    constexpr std::uint64_t kIncrement = 0x9E3779B97F4A7C15ULL;
    constexpr std::uint64_t kMul1 = 0xBF58476D1CE4E5B9ULL;
    constexpr std::uint64_t kMul2 = 0x94D049BB133111EBULL;
    constexpr int kShift1 = 30;
    constexpr int kShift2 = 27;
    constexpr int kShift3 = 31;
    state_ += kIncrement;
    std::uint64_t z = state_;
    z = (z ^ (z >> kShift1)) * kMul1;
    z = (z ^ (z >> kShift2)) * kMul2;
    return (z ^ (z >> kShift3)) % limit;
  }

 private:
  std::uint64_t state_;
};

Bytes bytes_of(std::string_view characters) {
  Bytes result;
  result.reserve(characters.size());
  for (const char c : characters) {
    result.push_back(static_cast<std::byte>(c));
  }
  return result;
}

// width * height pixels whose bytes count up, so a resized or shifted copy is noticed.
std::vector<std::uint8_t> counting_pixels(int width, int height) {
  std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
                                   static_cast<std::size_t>(height) * kChannels);
  for (std::size_t i = 0; i < pixels.size(); ++i) {
    pixels.at(i) = static_cast<std::uint8_t>(i);
  }
  return pixels;
}

SaveData sample_save() {
  SaveData data;
  data.engine_version = "0.1.0";
  data.game_version = "1.2.3";
  data.label = "start";
  data.step = 4;
  data.source_hash = kSomeHash;
  data.snapshot.label = "start";
  data.snapshot.store.tables = {
      StoreTable{.entries = {{std::string{"flag"}, true},
                             {std::string{"count"}, kSomeInteger},
                             {std::string{"ratio"}, kSomeNumber},
                             {std::string{"name"}, std::string{"Eileen \xE2\x9C\xBF"}},
                             {std::string{"bag"}, StoreTableRef{.index = 1}}}},
      StoreTable{.entries = {{std::int64_t{1}, std::string{"key"}},
                             {kSomeNumber, false},
                             {std::string{"again"}, StoreTableRef{.index = 1}}}}};
  data.snapshot.scene = bytes_of("scene");
  data.inputs = {MenuChoice{.index = 1}, MenuChoice{.index = 0}};
  data.persistent = "persistent";
  data.thumbnail = Thumbnail{.width = 2, .height = 1, .rgba = counting_pixels(2, 1)};
  data.time = kClockStart;
  data.title = "You came back.";
  return data;
}

Bytes read_bytes(const std::filesystem::path& path) {
  std::ifstream stream{path, std::ios::binary};
  Bytes bytes;
  for (std::istreambuf_iterator<char> it{stream}, end; it != end; ++it) {
    bytes.push_back(static_cast<std::byte>(*it));
  }
  return bytes;
}

void write_bytes(const std::filesystem::path& path, const Bytes& bytes) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream{path, std::ios::binary | std::ios::trunc};
  for (const std::byte b : bytes) {
    stream.put(static_cast<char>(b));
  }
}

// Decoding must give SaveData or SaveError, nothing else.
bool accepted(const Bytes& payload) {
  try {
    static_cast<void>(vnmaki::decode_save_payload(payload));
    return true;
  } catch (const SaveError&) {
    return false;
  }
}

std::string rejection(const SaveData& data) {
  try {
    static_cast<void>(vnmaki::decode_save_payload(vnmaki::encode_save_payload(data)));
  } catch (const SaveError& error) {
    return error.what();
  }
  return {};
}

}  // namespace

TEST_CASE("save file: a save reads back equal, and the same data gives the same bytes") {
  const SaveData data = sample_save();
  CHECK(vnmaki::decode_save_payload(vnmaki::encode_save_payload(data)) == data);
  CHECK(vnmaki::decode_save(vnmaki::encode_save(data)) == data);
  CHECK(vnmaki::encode_save(data) == vnmaki::encode_save(sample_save()));
}

TEST_CASE("save file: the answers of call_screen read back with their types") {
  SaveData data = sample_save();
  data.inputs = {ScreenResult{.value = ScreenValue{}},
                 ScreenResult{.value = ScreenValue{true}},
                 ScreenResult{.value = ScreenValue{kSomeInteger}},
                 ScreenResult{.value = ScreenValue{kSomeNumber}},
                 ScreenResult{.value = ScreenValue{std::string{"start"}}},
                 MenuChoice{.index = 2}};
  CHECK(vnmaki::decode_save_payload(vnmaki::encode_save_payload(data)) == data);
  data.inputs = {ScreenResult{.value = ScreenValue{std::int64_t{1}}}};
  CHECK(vnmaki::decode_save_payload(vnmaki::encode_save_payload(data)) == data);
  data.inputs = {ScreenResult{.value = ScreenValue{1.0}}};
  CHECK(vnmaki::decode_save_payload(vnmaki::encode_save_payload(data)) == data);
}

TEST_CASE("save file: integers of every size and a negative time survive") {
  SaveData data = sample_save();
  for (const std::int64_t time :
       {std::int64_t{0}, std::int64_t{-1}, std::int64_t{-32}, std::int64_t{-33}, std::int64_t{-129},
        std::int64_t{-40000}, std::numeric_limits<std::int64_t>::min(),
        std::numeric_limits<std::int64_t>::max()}) {
    data.time = time;
    CHECK(vnmaki::decode_save_payload(vnmaki::encode_save_payload(data)).time == time);
  }
  data.source_hash = std::numeric_limits<std::uint64_t>::max();
  data.step = std::numeric_limits<std::uint32_t>::max();
  const SaveData again = vnmaki::decode_save_payload(vnmaki::encode_save_payload(data));
  CHECK(again.source_hash == data.source_hash);
  CHECK(again.step == data.step);
}

TEST_CASE("save file: a store keeps its float and integer values apart") {
  SaveData data = sample_save();
  data.snapshot.store.tables = {StoreTable{.entries = {{std::string{"a"}, std::int64_t{1}},
                                                       {std::string{"b"}, 1.0},
                                                       {std::string{"c"}, -0.0}}}};
  const SaveData again = vnmaki::decode_save_payload(vnmaki::encode_save_payload(data));
  CHECK(
      std::holds_alternative<std::int64_t>(again.snapshot.store.tables.at(0).entries.at(0).second));
  CHECK(std::holds_alternative<double>(again.snapshot.store.tables.at(0).entries.at(1).second));
  CHECK(std::signbit(std::get<double>(again.snapshot.store.tables.at(0).entries.at(2).second)));
}

TEST_CASE("save file: keys this engine does not know are skipped") {
  Bytes payload = vnmaki::encode_save_payload(sample_save());
  REQUIRE(std::to_integer<std::uint8_t>(payload.front()) == kFixMap14 - 1);
  payload.front() = static_cast<std::byte>(kFixMap14);
  // "x": [1, 2]
  for (const int b : {0xA1, 0x78, 0x92, 0x01, 0x02}) {
    payload.push_back(static_cast<std::byte>(b));
  }
  CHECK(vnmaki::decode_save_payload(payload) == sample_save());
}

TEST_CASE("save file: bad files are rejected with SaveError") {
  CHECK_THROWS_AS(static_cast<void>(vnmaki::decode_save({})), SaveError);
  CHECK_THROWS_AS(static_cast<void>(vnmaki::decode_save(Bytes(64, std::byte{0x41}))), SaveError);
  Bytes file = vnmaki::encode_save(sample_save());
  file.resize(file.size() / 2);
  CHECK_THROWS_AS(static_cast<void>(vnmaki::decode_save(file)), SaveError);
}

TEST_CASE("save file: every cut of a payload is rejected, and no edit of it crashes") {
  const Bytes payload = vnmaki::encode_save_payload(sample_save());
  for (std::size_t length = 0; length < payload.size(); ++length) {
    CHECK_FALSE(
        accepted(Bytes(payload.begin(), payload.begin() + static_cast<std::ptrdiff_t>(length))));
  }
  Bytes extended = payload;
  extended.push_back(std::byte{0});
  CHECK_FALSE(accepted(extended));
  for (std::size_t i = 0; i < payload.size(); ++i) {
    Bytes edited = payload;
    edited.at(i) ^= static_cast<std::byte>(kByteFlip);
    static_cast<void>(accepted(edited));
  }
}

TEST_CASE("save file: random bytes are rejected without a crash") {
  Random random{kSeed};
  for (int i = 0; i < kRandomInputs; ++i) {
    Bytes bytes(random.below(kMaxRandomLength));
    for (std::byte& b : bytes) {
      b = static_cast<std::byte>(random.below(std::numeric_limits<std::uint8_t>::max() + 1U));
    }
    static_cast<void>(accepted(bytes));
    CHECK_THROWS_AS(static_cast<void>(vnmaki::decode_save(bytes)), SaveError);
  }
}

TEST_CASE("save file: data that replay could not take is rejected") {
  SaveData data = sample_save();
  CHECK(rejection(data).empty());

  data = sample_save();
  data.format = vnmaki::kSaveFormatVersion + 1;
  CHECK(rejection(data).find("format") != std::string::npos);

  data = sample_save();
  data.snapshot.store.tables.at(1).entries.at(2).second = StoreTableRef{.index = 2};
  CHECK(rejection(data).find("table") != std::string::npos);

  data = sample_save();
  data.snapshot.store.tables.clear();
  CHECK(rejection(data).find("no tables") != std::string::npos);

  data = sample_save();
  data.snapshot.store.tables.at(0).entries.at(0).first = StoreTableRef{.index = 0};
  CHECK_FALSE(rejection(data).empty());

  data = sample_save();
  data.snapshot.store.tables.at(0).entries.at(0).first = std::nan("");
  CHECK(rejection(data).find("NaN") != std::string::npos);

  data = sample_save();
  data.thumbnail.rgba.pop_back();
  CHECK(rejection(data).find("thumbnail") != std::string::npos);

  data = sample_save();
  data.thumbnail = Thumbnail{.width = vnmaki::kMaxThumbnailSide + 1, .height = 1, .rgba = {}};
  CHECK(rejection(data).find("thumbnail") != std::string::npos);
}

TEST_CASE("thumbnail: an image shrinks to the thumbnail width by averaging") {
  constexpr int kSide = 384;  // twice kThumbnailWidth
  vnmaki::Image image{.size = {.width = kSide, .height = kSide / 2}, .pixels = {}};
  image.pixels.reserve(static_cast<std::size_t>(kSide) * (kSide / 2) * kChannels);
  for (int y = 0; y < kSide / 2; ++y) {
    for (int x = 0; x < kSide; ++x) {
      // Pairs of columns hold 100 and 200, so each 2x2 block averages to 150.
      const std::uint8_t value = x % 2 == 0 ? 100 : 200;
      image.pixels.insert(image.pixels.end(), {value, value, value, UINT8_MAX});
    }
  }
  const Thumbnail thumbnail = vnmaki::make_thumbnail(image);
  CHECK(thumbnail.width == vnmaki::kThumbnailWidth);
  CHECK(thumbnail.height == vnmaki::kThumbnailWidth / 2);
  REQUIRE(thumbnail.rgba.size() == static_cast<std::size_t>(thumbnail.width) *
                                       static_cast<std::size_t>(thumbnail.height) * kChannels);
  CHECK(thumbnail.rgba.at(0) == 150);
  CHECK(thumbnail.rgba.at(3) == UINT8_MAX);
}

TEST_CASE("thumbnail: a small image keeps its size, a bad image gives an empty thumbnail") {
  vnmaki::Image image{.size = {.width = 2, .height = 1}, .pixels = counting_pixels(2, 1)};
  const Thumbnail thumbnail = vnmaki::make_thumbnail(image);
  CHECK(thumbnail.width == 2);
  CHECK(thumbnail.height == 1);
  CHECK(thumbnail.rgba == image.pixels);

  image.pixels.pop_back();
  CHECK(vnmaki::make_thumbnail(image) == Thumbnail{});
  CHECK(vnmaki::make_thumbnail({}) == Thumbnail{});
}

TEST_CASE("thumbnail: the stub is a valid thumbnail") {
  const Thumbnail stub = vnmaki::stub_thumbnail();
  CHECK(stub.rgba.size() ==
        static_cast<std::size_t>(stub.width) * static_cast<std::size_t>(stub.height) * kChannels);
  SaveData data = sample_save();
  data.thumbnail = stub;
  CHECK(rejection(data).empty());
}

namespace {

// Three lines and a menu: steps 1 "one", 2 the menu, 3 "three", 4 "four".
constexpr std::string_view kStory = R"(
label("start", function()
  store.visits = (store.visits or 0) + 1
  narrate("one")
  store.pick = menu({ "left", "right" })
  narrate("three")
  narrate("four")
end)
)";

// One game: its scripts, runtime and saves in `directory`.
struct Game {
  explicit Game(const std::string& source, const std::filesystem::path& directory,
                std::string game_version = "1.0.0", std::vector<AutoSaveEvent> auto_save = {},
                vnmaki::ScriptRuntimeOptions runtime_options = {})
      : vfs(vnmaki::test::memory_vfs({{"scripts/main.lua", source}})),
        runtime(vfs, runtime_options),
        saves(runtime, make_options(directory, std::move(game_version), std::move(auto_save))) {
    runtime.load_scripts();
  }

  SavesOptions make_options(const std::filesystem::path& directory, std::string game_version,
                            std::vector<AutoSaveEvent> auto_save) {
    SavesOptions options;
    options.directory = directory;
    options.game_version = std::move(game_version);
    options.auto_save = std::move(auto_save);
    options.clock = [this] { return kClockStart + clock_ticks++; };
    options.on_warning = [this](std::string_view message) { warnings.emplace_back(message); };
    return options;
  }

  [[nodiscard]] std::string narrated() const {
    const auto* narrate = std::get_if<NarrateRequest>(runtime.request());
    return narrate != nullptr ? narrate->text : std::string{};
  }

  // The string stored under `key` in the store, or empty.
  [[nodiscard]] std::string stored_text(std::string_view key) const {
    const StoreSnapshot snapshot = runtime.store_snapshot();
    for (const auto& [name, value] : snapshot.tables.at(0).entries) {
      if (const auto* text = std::get_if<std::string>(&name); text != nullptr && *text == key) {
        const auto* result = std::get_if<std::string>(&value);
        return result != nullptr ? *result : std::string{};
      }
    }
    return {};
  }

  vnmaki::Vfs vfs;
  vnmaki::ScriptRuntime runtime;
  vnmaki::Saves saves;
  std::vector<std::string> warnings;
  std::int64_t clock_ticks = 0;
};

// Starts kStory and plays to the narrate "three" (step 3), having chosen "right".
void play_to_three(Game& game) {
  game.runtime.start("start");
  game.runtime.advance();
  game.runtime.choose(1);
}

}  // namespace

TEST_CASE("saves: a slot saves a position and loading brings it back") {
  const TempDir dir{"round_trip"};
  Game game{std::string{kStory}, dir.path()};
  play_to_three(game);
  REQUIRE(game.narrated() == "three");
  const StoreSnapshot saved_store = game.runtime.store_snapshot();
  CHECK(game.saves.save("1").empty());
  CHECK(game.saves.exists("1"));

  game.runtime.advance();
  REQUIRE(game.narrated() == "four");

  CHECK(game.saves.load("1").empty());
  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 3});
  CHECK(game.narrated() == "three");
  CHECK(game.runtime.store_snapshot() == saved_store);
  CHECK(game.warnings.empty());

  game.runtime.advance();
  CHECK(game.narrated() == "four");
  game.runtime.advance();
  CHECK(game.runtime.state() == RunState::kFinished);
}

TEST_CASE("saves: a save loads into a runtime that has not started") {
  const TempDir dir{"fresh_runtime"};
  Game first{std::string{kStory}, dir.path()};
  play_to_three(first);
  CHECK(first.saves.save("7").empty());

  Game second{std::string{kStory}, dir.path()};
  CHECK(second.saves.load("7").empty());
  CHECK(second.runtime.position() == ScriptPosition{.label = "start", .step = 3});
  CHECK(second.narrated() == "three");
}

TEST_CASE("saves: the file holds the fields of SPEC section 8") {
  const TempDir dir{"fields"};
  Game game{std::string{kStory}, dir.path(), "1.4.0"};
  play_to_three(game);
  CHECK(game.saves.save("2").empty());

  const SaveData data = vnmaki::decode_save(read_bytes(dir.path() / "2.vnsave"));
  CHECK(data.engine_version == "0.1.0");
  CHECK(data.game_version == "1.4.0");
  CHECK(data.label == "start");
  CHECK(data.step == 3);
  CHECK(data.source_hash == game.runtime.label_source_hash("start"));
  CHECK(data.inputs == std::vector<vnmaki::InputResult>{MenuChoice{.index = 1}});
  CHECK(data.snapshot.store == game.runtime.label_snapshot()->store);
  CHECK(data.persistent == "persistent");
  CHECK(data.thumbnail == vnmaki::stub_thumbnail());
  CHECK(data.time == kClockStart);
  CHECK(data.title == "three");
}

TEST_CASE("saves: a save needs a waiting story and a slot name that is safe") {
  const TempDir dir{"refusals"};
  Game game{std::string{kStory}, dir.path()};
  CHECK_FALSE(game.saves.save("1").empty());  // not started
  game.runtime.start("start");
  CHECK_FALSE(game.saves.save("../evil").empty());
  CHECK_FALSE(game.saves.save("").empty());
  CHECK_FALSE(game.saves.save("A").empty());
  CHECK_FALSE(game.saves.load("a/b").empty());
  CHECK_FALSE(game.saves.remove("..").empty());
  game.runtime.advance();
  game.runtime.choose(0);
  game.runtime.advance();
  game.runtime.advance();
  REQUIRE(game.runtime.state() == RunState::kFinished);
  CHECK_FALSE(game.saves.save("1").empty());
  CHECK_FALSE(std::filesystem::exists(dir.path()));
}

TEST_CASE("saves: quick save, quick load and auto save use their own slots") {
  const TempDir dir{"named_slots"};
  Game game{std::string{kStory}, dir.path()};
  game.runtime.start("start");
  CHECK(game.saves.quick_save().empty());
  game.runtime.advance();
  CHECK(game.saves.auto_save().empty());
  CHECK(game.saves.exists("quick"));
  CHECK(game.saves.exists("auto"));
  CHECK_FALSE(game.saves.exists("1"));

  CHECK(game.saves.quick_load().empty());
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
  CHECK(game.saves.load("auto").empty());
  CHECK(game.runtime.position() == ScriptPosition{.label = "start", .step = 2});
}

TEST_CASE("saves: list shows numbered slots in order, then named ones, and bad files") {
  const TempDir dir{"listing"};
  Game game{std::string{kStory}, dir.path()};
  CHECK(game.saves.list().empty());
  game.runtime.start("start");
  for (const std::string_view slot : {"10", "2", "quick", "1"}) {
    CHECK(game.saves.save(slot).empty());
  }
  write_bytes(dir.path() / "3.vnsave", Bytes(kGarbageSize, kGarbageByte));
  write_bytes(dir.path() / "notes.txt", Bytes(kGarbageSize, kGarbageByte));

  const std::vector<vnmaki::SlotInfo> slots = game.saves.list();
  REQUIRE(slots.size() == 5);
  const std::vector<std::string> names{"1", "2", "3", "10", "quick"};
  for (std::size_t i = 0; i < names.size(); ++i) {
    CHECK(slots.at(i).slot == names.at(i));
  }
  CHECK(slots.at(0).error.empty());
  CHECK(slots.at(0).title == "one");
  CHECK(slots.at(0).label == "start");
  CHECK(slots.at(0).game_version == "1.0.0");
  CHECK(slots.at(0).time == kClockStart + 3);  // "10", "2", "quick" were saved first
  CHECK(slots.at(0).thumbnail == vnmaki::stub_thumbnail());
  CHECK_FALSE(slots.at(2).error.empty());

  CHECK(game.saves.remove("2").empty());
  CHECK(game.saves.remove("2").empty());
  CHECK_FALSE(game.saves.exists("2"));
  CHECK(game.saves.list().size() == 4);
}

TEST_CASE("saves: a new save replaces the old one") {
  const TempDir dir{"overwrite"};
  Game game{std::string{kStory}, dir.path()};
  game.runtime.start("start");
  CHECK(game.saves.save("1").empty());
  game.runtime.advance();
  CHECK(game.saves.save("1").empty());
  CHECK(game.saves.list().size() == 1);
  CHECK(game.saves.load("1").empty());
  CHECK(game.runtime.position().step == 2);
}

TEST_CASE("saves: a load that fails leaves the story where it was") {
  const TempDir dir{"bad_loads"};
  Game game{std::string{kStory}, dir.path()};
  game.runtime.start("start");
  game.runtime.advance();
  const ScriptPosition before = game.runtime.position();

  CHECK(game.saves.load("1").find("empty") != std::string::npos);
  write_bytes(dir.path() / "5.vnsave", Bytes(kGarbageSize, kGarbageByte));
  CHECK_FALSE(game.saves.load("5").empty());
  Bytes truncated = vnmaki::encode_save(sample_save());
  truncated.resize(truncated.size() - 3);
  write_bytes(dir.path() / "6.vnsave", truncated);
  CHECK_FALSE(game.saves.load("6").empty());
  // A good file for a label this game does not have.
  SaveData other = sample_save();
  other.label = "gone";
  other.snapshot.label = "gone";
  write_bytes(dir.path() / "8.vnsave", vnmaki::encode_save(other));
  CHECK(game.saves.load("8").find("no longer exists") != std::string::npos);

  CHECK(game.runtime.state() == RunState::kWaiting);
  CHECK(game.runtime.position() == before);
}

TEST_CASE("saves: a label whose source changed restarts from its first line with a warning") {
  const TempDir dir{"changed_label"};
  Game old_game{std::string{kStory}, dir.path()};
  play_to_three(old_game);
  CHECK(old_game.saves.save("1").empty());

  constexpr std::string_view kOldLine = "narrate(\"four\")";
  std::string edited{kStory};
  edited.replace(edited.find(kOldLine), kOldLine.size(), "narrate(\"four!\")");
  Game new_game{edited, dir.path()};
  CHECK(new_game.saves.load("1").empty());
  CHECK(new_game.runtime.position() == ScriptPosition{.label = "start", .step = 1});
  CHECK(new_game.narrated() == "one");
  REQUIRE(new_game.warnings.size() == 1);
  CHECK(new_game.warnings.at(0).find("changed") != std::string::npos);
  CHECK(new_game.warnings.at(0).find("start") != std::string::npos);
}

TEST_CASE("saves: a load that cannot be replayed goes back to the earlier position") {
  // The helper is outside the label function, so editing it does not change the source hash.
  const auto story = [](std::string_view helper_lines) {
    return "local function intro()\n" + std::string{helper_lines} +
           "end\n"
           "label(\"start\", function()\n"
           "  intro()\n"
           "  narrate(\"after\")\n"
           "end)\n";
  };
  const TempDir dir{"replay_fails"};
  Game old_game{story("narrate('a')\nnarrate('b')\n"), dir.path()};
  old_game.runtime.start("start");
  old_game.runtime.advance();
  old_game.runtime.advance();
  REQUIRE(old_game.narrated() == "after");
  CHECK(old_game.saves.save("1").empty());

  Game new_game{story("narrate('a')\n"), dir.path()};
  new_game.runtime.start("start");
  const ScriptPosition before = new_game.runtime.position();
  const std::string problem = new_game.saves.load("1");
  CHECK(problem.find("could not follow") != std::string::npos);
  CHECK(new_game.runtime.state() == RunState::kWaiting);
  CHECK(new_game.runtime.position() == before);
  CHECK(new_game.narrated() == "a");
}

TEST_CASE("saves: a load that cannot be replayed keeps the callers and the history") {
  // The helper is outside the label function, so editing it does not change the source hash.
  const auto story = [](std::string_view helper_lines) {
    return "local function intro()\n" + std::string{helper_lines} + R"(end
label("start", function()
  narrate("s1")
  call("aside")
  narrate("s2")
end)
label("aside", function()
  intro()
  narrate("after")
end)
)";
  };
  const TempDir dir{"replay_fails_callers"};
  Game old_game{story("narrate('a')\nnarrate('b')\n"), dir.path()};
  old_game.runtime.start("start");
  old_game.runtime.advance();
  old_game.runtime.advance();
  old_game.runtime.advance();
  REQUIRE(old_game.narrated() == "after");
  CHECK(old_game.saves.save("1").empty());

  Game new_game{story("narrate('a')\n"), dir.path()};
  new_game.runtime.start("start");
  new_game.runtime.advance();
  REQUIRE(new_game.narrated() == "a");
  const std::size_t history = new_game.runtime.rollback_steps().size();

  CHECK(new_game.saves.load("1").find("could not follow") != std::string::npos);
  CHECK(new_game.narrated() == "a");
  CHECK(new_game.runtime.rollback_steps().size() == history);
  CHECK(new_game.runtime.can_roll_back());
  new_game.runtime.advance();
  CHECK(new_game.narrated() == "after");
  new_game.runtime.advance();
  CHECK(new_game.narrated() == "s2");  // the caller is still there
  CHECK(new_game.runtime.roll_back());
  CHECK(new_game.narrated() == "after");
}

TEST_CASE("saves: a label that used call_screen saves and loads with the screen's answer") {
  constexpr std::string_view kScreenStory = R"(
screen "pick" { button { "A", returns = "a" } }
label("start", function()
  local r = call_screen("pick")
  narrate("got " .. r)
  narrate("end")
end)
)";
  const TempDir dir{"call_screen"};
  Game game{std::string{kScreenStory}, dir.path()};
  game.runtime.start("start");
  game.runtime.return_screen(ScreenValue{std::string{"a"}});
  REQUIRE(game.narrated() == "got a");
  CHECK(game.saves.save("1").empty());
  game.runtime.advance();

  CHECK(game.saves.load("1").empty());
  CHECK(game.narrated() == "got a");
  CHECK(game.warnings.empty());
}

TEST_CASE("saves: a load drops the labels that called the running label") {
  constexpr std::string_view kCallStory = R"(
label("start", function()
  narrate("s1")
  call("aside")
  narrate("s2")
end)
label("aside", function()
  narrate("a1")
end)
)";
  const TempDir dir{"callers"};
  Game game{std::string{kCallStory}, dir.path()};
  game.runtime.start("start");
  CHECK(game.saves.save("1").empty());
  game.runtime.advance();
  REQUIRE(game.runtime.position().label == "aside");

  CHECK(game.saves.load("1").empty());
  CHECK(game.narrated() == "s1");
  game.runtime.advance();
  CHECK(game.narrated() == "a1");
  game.runtime.advance();
  CHECK(game.narrated() == "s2");
  game.runtime.advance();
  CHECK(game.runtime.state() == RunState::kFinished);
}

TEST_CASE("saves: on_load migrates the saved store before the story goes on") {
  constexpr std::string_view kMigrating = R"(
on_load(function(old_version, store)
  store.migrated_from = old_version
  store.pick = (store.pick or 0) * 10
end)
label("start", function()
  narrate("one")
  store.pick = menu({ "left", "right" })
  narrate("three")
  store.seen_pick = tostring(store.pick) .. "/" .. tostring(store.migrated_from)
  narrate("four")
end)
)";
  const TempDir dir{"migration"};
  Game old_game{std::string{kMigrating}, dir.path(), "1.0.0"};
  old_game.runtime.start("start");
  old_game.runtime.advance();
  old_game.runtime.choose(1);  // store.pick = 2
  CHECK(old_game.saves.save("1").empty());

  Game new_game{std::string{kMigrating}, dir.path(), "2.0.0"};
  CHECK(new_game.saves.load("1").empty());
  CHECK(new_game.runtime.position() == ScriptPosition{.label = "start", .step = 3});
  // The hook ran on the label-entry store, where `pick` was not set yet, so it saw 0 and wrote
  // `migrated_from`; the replay then ran the label again with that store.
  CHECK(new_game.stored_text("migrated_from") == "1.0.0");
  new_game.runtime.advance();
  CHECK(new_game.stored_text("seen_pick") == "2/1.0.0");
}

TEST_CASE("saves: a failing or endless on_load refuses the load and changes nothing") {
  const TempDir dir{"bad_hooks"};
  const auto story = [](std::string_view hook_body) {
    return "on_load(function(old_version, store) " + std::string{hook_body} +
           " end)\n"
           "label(\"start\", function()\n"
           "  store.mark = 'live'\n"
           "  narrate('one')\n"
           "  narrate('two')\n"
           "end)\n";
  };
  Game plain{story(""), dir.path()};
  plain.runtime.start("start");
  CHECK(plain.saves.save("1").empty());

  Game raising{story("error('boom')"), dir.path()};
  raising.runtime.start("start");
  raising.runtime.advance();
  const ScriptPosition before = raising.runtime.position();
  CHECK(raising.saves.load("1").find("boom") != std::string::npos);
  CHECK(raising.runtime.position() == before);
  CHECK(raising.stored_text("mark") == "live");

  constexpr std::uint32_t kLimit = 20000;
  Game endless{story("while true do end"),
               dir.path(),
               "1.0.0",
               {},
               vnmaki::ScriptRuntimeOptions{.instruction_limit = kLimit}};
  endless.runtime.start("start");
  CHECK(endless.saves.load("1").find("on_load") != std::string::npos);
  CHECK(endless.runtime.state() == RunState::kWaiting);

  Game yielding{story("narrate('no')"), dir.path()};
  yielding.runtime.start("start");
  CHECK_FALSE(yielding.saves.load("1").empty());
  CHECK(yielding.runtime.state() == RunState::kWaiting);
}

TEST_CASE("saves: scripts ask for saves and loads, and poll carries them out") {
  constexpr std::string_view kAsking = R"(
label("start", function()
  store.code = load("return 41 + 1")()
  narrate("one")
  save(2)
  narrate("two")
  quick_save()
  narrate("three")
  quick_load()
  narrate("after")
end)
)";
  const TempDir dir{"script_calls"};
  Game game{std::string{kAsking}, dir.path()};
  game.runtime.start("start");
  game.saves.poll();
  CHECK_FALSE(game.saves.exists("2"));

  game.runtime.advance();  // save(2) ran; waits at "two"
  game.saves.poll();
  CHECK(game.saves.exists("2"));
  CHECK(game.saves.load("2").empty());
  CHECK(game.narrated() == "two");
  CHECK(game.runtime.position().step == 2);

  game.runtime.advance();  // quick_save() ran; waits at "three"
  game.saves.poll();
  CHECK(game.saves.exists("quick"));
  game.runtime.advance();  // quick_load() ran; waits at "after"
  CHECK(game.narrated() == "after");
  game.saves.poll();  // loads the quick save
  CHECK(game.narrated() == "three");
  CHECK(game.runtime.position().step == 3);
  CHECK(game.warnings.empty());
}

TEST_CASE("saves: a script's load(slot) asks for a load and load(text) is still Lua's load") {
  constexpr std::string_view kLoading = R"(
label("start", function()
  narrate("one")
  load(4)
  narrate("two")
end)
)";
  const TempDir dir{"script_load"};
  Game game{std::string{kLoading}, dir.path()};
  game.runtime.start("start");
  CHECK(game.saves.save("4").empty());
  game.runtime.advance();
  CHECK(game.narrated() == "two");
  game.saves.poll();
  CHECK(game.narrated() == "one");
  CHECK(game.runtime.position().step == 1);
}

TEST_CASE("saves: a request that fails is reported as a warning") {
  const TempDir dir{"failed_request"};
  Game game{std::string{"label('start', function() narrate('a') load(9) narrate('b') end)"},
            dir.path()};
  game.runtime.start("start");
  game.runtime.advance();
  game.saves.poll();
  REQUIRE(game.warnings.size() == 1);
  CHECK(game.warnings.at(0).find("empty") != std::string::npos);
  CHECK(game.narrated() == "b");
}

TEST_CASE("saves: bad slot arguments are script errors at the calling line") {
  const TempDir dir{"bad_arguments"};
  for (const std::string_view call :
       {"save(0)", "save('a')", "save(1.5)", "load(-1)", "save(1000000)", "save()"}) {
    Game game{"label('start', function()\n" + std::string{call} + "\nnarrate('x')\nend)",
              dir.path()};
    game.runtime.start("start");
    CHECK(game.runtime.state() == RunState::kFailed);
    CHECK(game.runtime.error().find("main.lua:2") != std::string::npos);
  }
}

TEST_CASE("saves: requests made while a replay runs are dropped") {
  const TempDir dir{"replay_requests"};
  Game game{std::string{"label('start', function() save(3) narrate('a') narrate('b') end)"},
            dir.path()};
  game.runtime.start("start");
  game.saves.poll();
  CHECK(game.saves.exists("3"));
  game.runtime.advance();
  CHECK(game.saves.save("1").empty());
  // Loading replays the label, which calls save(3) again; that must not queue a request.
  CHECK(game.saves.load("1").empty());
  CHECK(game.runtime.take_save_requests().empty());
}

TEST_CASE("saves: auto save follows the events of project.toml, once per position") {
  constexpr std::string_view kTwoLabels = R"(
label("start", function()
  narrate("one")
  narrate("two")
  jump("second")
end)
label("second", function()
  local pick = menu({ "a", "b" })
  narrate("end")
end)
)";
  SUBCASE("label events save at the first line of each label") {
    const TempDir dir{"auto_label"};
    Game game{std::string{kTwoLabels}, dir.path(), "1.0.0", {AutoSaveEvent::kLabel}};
    game.runtime.start("start");
    game.saves.poll();
    game.saves.poll();  // the same position again
    REQUIRE(game.saves.exists("auto"));
    CHECK(vnmaki::decode_save(read_bytes(dir.path() / "auto.vnsave")).time == kClockStart);

    game.runtime.advance();
    game.saves.poll();  // step 2: not an event
    game.runtime.advance();
    game.saves.poll();  // the menu of "second" is step 1 of that label
    CHECK(vnmaki::decode_save(read_bytes(dir.path() / "auto.vnsave")).label == "second");
    CHECK(vnmaki::decode_save(read_bytes(dir.path() / "auto.vnsave")).time == kClockStart + 1);
  }
  SUBCASE("menu events save only at menus") {
    const TempDir dir{"auto_menu"};
    Game game{std::string{kTwoLabels}, dir.path(), "1.0.0", {AutoSaveEvent::kMenu}};
    game.runtime.start("start");
    game.saves.poll();
    CHECK_FALSE(game.saves.exists("auto"));
    game.runtime.advance();
    game.runtime.advance();
    game.saves.poll();
    CHECK(game.saves.exists("auto"));
  }
  SUBCASE("without events nothing is saved") {
    const TempDir dir{"auto_none"};
    Game game{std::string{kTwoLabels}, dir.path()};
    game.runtime.start("start");
    game.saves.poll();
    CHECK_FALSE(std::filesystem::exists(dir.path()));
  }
  SUBCASE("loading a save does not auto save its position again") {
    const TempDir dir{"auto_load"};
    Game game{std::string{kTwoLabels}, dir.path(), "1.0.0", {AutoSaveEvent::kLabel}};
    game.runtime.start("start");
    CHECK(game.saves.save("1").empty());
    CHECK(game.saves.load("1").empty());
    game.saves.poll();
    CHECK_FALSE(game.saves.exists("auto"));
  }
}

TEST_CASE("project.toml: auto_save lists the events that write the auto-save slot") {
  CHECK(vnmaki::parse_project_config("title = \"A\"\n").auto_save.empty());
  const vnmaki::ProjectConfig config =
      vnmaki::parse_project_config("title = \"A\"\nauto_save = [\"menu\", \"label\"]\n");
  CHECK(config.auto_save ==
        std::vector<AutoSaveEvent>{AutoSaveEvent::kMenu, AutoSaveEvent::kLabel});
  CHECK_THROWS_AS(
      static_cast<void>(vnmaki::parse_project_config("title = \"A\"\nauto_save = [\"quit\"]\n")),
      vnmaki::ProjectConfigError);
  CHECK_THROWS_AS(
      static_cast<void>(vnmaki::parse_project_config("title = \"A\"\nauto_save = \"label\"\n")),
      vnmaki::ProjectConfigError);
  CHECK_THROWS_AS(static_cast<void>(vnmaki::parse_project_config(
                      "title = \"A\"\nauto_save = [\"label\", \"label\"]\n")),
                  vnmaki::ProjectConfigError);
}
