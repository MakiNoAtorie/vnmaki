#include "engine/saves/save_file.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <zstd.h>

namespace vnmaki {

namespace {

// ---- MessagePack ---------------------------------------------------------------------------
// Only what the save format needs: nil is never written and extension types are rejected.

constexpr unsigned kBitsPerByte = 8;
constexpr std::uint64_t kByteMask = 0xFF;

constexpr std::uint8_t kPositiveFixMax = 0x7F;
constexpr std::uint8_t kNegativeFixMin = 0xE0;
constexpr std::int64_t kNegativeFixLowest = -32;
constexpr std::uint8_t kFixMapBase = 0x80;
constexpr std::uint8_t kFixArrayBase = 0x90;
constexpr std::uint8_t kFixStrBase = 0xA0;
constexpr std::uint8_t kFixKindMask = 0xF0;
constexpr std::uint8_t kFixStrKindMask = 0xE0;
constexpr std::uint8_t kFixCountMask = 0x0F;
constexpr std::uint8_t kFixStrCountMask = 0x1F;
constexpr std::size_t kFixArrayMax = 15;
constexpr std::size_t kFixStrMax = 31;

constexpr std::uint8_t kNil = 0xC0;
constexpr std::uint8_t kFalse = 0xC2;
constexpr std::uint8_t kTrue = 0xC3;
constexpr std::uint8_t kBin8 = 0xC4;
constexpr std::uint8_t kBin16 = 0xC5;
constexpr std::uint8_t kBin32 = 0xC6;
constexpr std::uint8_t kFloat32 = 0xCA;
constexpr std::uint8_t kFloat64 = 0xCB;
constexpr std::uint8_t kUint8 = 0xCC;
constexpr std::uint8_t kUint16 = 0xCD;
constexpr std::uint8_t kUint32 = 0xCE;
constexpr std::uint8_t kUint64 = 0xCF;
constexpr std::uint8_t kInt8 = 0xD0;
constexpr std::uint8_t kInt16 = 0xD1;
constexpr std::uint8_t kInt32 = 0xD2;
constexpr std::uint8_t kInt64 = 0xD3;
constexpr std::uint8_t kStr8 = 0xD9;
constexpr std::uint8_t kStr16 = 0xDA;
constexpr std::uint8_t kStr32 = 0xDB;
constexpr std::uint8_t kArray16 = 0xDC;
constexpr std::uint8_t kArray32 = 0xDD;
constexpr std::uint8_t kMap16 = 0xDE;
constexpr std::uint8_t kMap32 = 0xDF;

constexpr std::size_t kBytes1 = 1;
constexpr std::size_t kBytes2 = 2;
constexpr std::size_t kBytes4 = 4;
constexpr std::size_t kBytes8 = 8;

// How deep skip() follows arrays and maps of keys this engine does not know.
constexpr int kMaxSkipDepth = 16;

class Writer {
 public:
  void unsigned_integer(std::uint64_t value) {
    if (value <= kPositiveFixMax) {
      byte(static_cast<std::uint8_t>(value));
    } else if (value <= std::numeric_limits<std::uint8_t>::max()) {
      byte(kUint8);
      big_endian(value, kBytes1);
    } else if (value <= std::numeric_limits<std::uint16_t>::max()) {
      byte(kUint16);
      big_endian(value, kBytes2);
    } else if (value <= std::numeric_limits<std::uint32_t>::max()) {
      byte(kUint32);
      big_endian(value, kBytes4);
    } else {
      byte(kUint64);
      big_endian(value, kBytes8);
    }
  }

  void integer(std::int64_t value) {
    if (value >= 0) {
      unsigned_integer(static_cast<std::uint64_t>(value));
    } else if (value >= kNegativeFixLowest) {
      byte(static_cast<std::uint8_t>(value));
    } else if (value >= std::numeric_limits<std::int8_t>::min()) {
      byte(kInt8);
      big_endian(static_cast<std::uint64_t>(value), kBytes1);
    } else if (value >= std::numeric_limits<std::int16_t>::min()) {
      byte(kInt16);
      big_endian(static_cast<std::uint64_t>(value), kBytes2);
    } else if (value >= std::numeric_limits<std::int32_t>::min()) {
      byte(kInt32);
      big_endian(static_cast<std::uint64_t>(value), kBytes4);
    } else {
      byte(kInt64);
      big_endian(static_cast<std::uint64_t>(value), kBytes8);
    }
  }

  void nil() { byte(kNil); }
  void boolean(bool value) { byte(value ? kTrue : kFalse); }

  void number(double value) {
    byte(kFloat64);
    big_endian(std::bit_cast<std::uint64_t>(value), kBytes8);
  }

  void str(std::string_view text) {
    if (text.size() <= kFixStrMax) {
      byte(static_cast<std::uint8_t>(kFixStrBase | text.size()));
    } else {
      sized(text.size(), kStr8, kStr16, kStr32);
    }
    for (const char c : text) {
      bytes_.push_back(static_cast<std::byte>(c));
    }
  }

  void bin(std::span<const std::byte> data) {
    sized(data.size(), kBin8, kBin16, kBin32);
    bytes_.insert(bytes_.end(), data.begin(), data.end());
  }

  void array(std::size_t count) { container(count, kFixArrayBase, kArray16, kArray32); }
  void map(std::size_t count) { container(count, kFixMapBase, kMap16, kMap32); }

  [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  void byte(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }

  void big_endian(std::uint64_t value, std::size_t size) {
    for (std::size_t i = size; i > 0; --i) {
      byte(static_cast<std::uint8_t>((value >> ((i - 1) * kBitsPerByte)) & kByteMask));
    }
  }

  // The marker for a str or bin of `size` bytes, then its length.
  void sized(std::size_t size, std::uint8_t marker8, std::uint8_t marker16, std::uint8_t marker32) {
    if (size <= std::numeric_limits<std::uint8_t>::max()) {
      byte(marker8);
      big_endian(size, kBytes1);
    } else if (size <= std::numeric_limits<std::uint16_t>::max()) {
      byte(marker16);
      big_endian(size, kBytes2);
    } else {
      byte(marker32);
      big_endian(size, kBytes4);
    }
  }

  void container(std::size_t count, std::uint8_t fix_base, std::uint8_t marker16,
                 std::uint8_t marker32) {
    if (count <= kFixArrayMax) {
      byte(static_cast<std::uint8_t>(fix_base | count));
    } else if (count <= std::numeric_limits<std::uint16_t>::max()) {
      byte(marker16);
      big_endian(count, kBytes2);
    } else {
      byte(marker32);
      big_endian(count, kBytes4);
    }
  }

  std::vector<std::byte> bytes_;
};

[[noreturn]] void fail(std::string_view reason) { throw SaveError{"save: " + std::string{reason}}; }

enum class Kind : std::uint8_t { kBool, kInteger, kFloat, kString, kBinary, kArray, kMap, kOther };

Kind kind_of(std::uint8_t marker) {
  if (marker <= kPositiveFixMax || marker >= kNegativeFixMin) {
    return Kind::kInteger;
  }
  if ((marker & kFixKindMask) == kFixMapBase) {
    return Kind::kMap;
  }
  if ((marker & kFixKindMask) == kFixArrayBase) {
    return Kind::kArray;
  }
  if ((marker & kFixStrKindMask) == kFixStrBase) {
    return Kind::kString;
  }
  switch (marker) {
    case kFalse:
    case kTrue:
      return Kind::kBool;
    case kFloat32:
    case kFloat64:
      return Kind::kFloat;
    case kUint8:
    case kUint16:
    case kUint32:
    case kUint64:
    case kInt8:
    case kInt16:
    case kInt32:
    case kInt64:
      return Kind::kInteger;
    case kStr8:
    case kStr16:
    case kStr32:
      return Kind::kString;
    case kBin8:
    case kBin16:
    case kBin32:
      return Kind::kBinary;
    case kArray16:
    case kArray32:
      return Kind::kArray;
    case kMap16:
    case kMap32:
      return Kind::kMap;
    default:
      return Kind::kOther;
  }
}

class Reader {
 public:
  explicit Reader(std::span<const std::byte> data) : data_(data) {}

  [[nodiscard]] bool at_end() const { return position_ == data_.size(); }
  [[nodiscard]] std::size_t remaining() const { return data_.size() - position_; }
  [[nodiscard]] Kind peek_kind() const { return kind_of(peek_byte()); }
  [[nodiscard]] bool peek_nil() const { return peek_byte() == kNil; }
  void nil() {
    if (next() != kNil) {
      fail("expected nil");
    }
  }

  bool boolean() {
    const std::uint8_t marker = next();
    if (marker != kTrue && marker != kFalse) {
      fail("expected a boolean");
    }
    return marker == kTrue;
  }

  std::int64_t integer() {
    const std::uint8_t marker = next();
    if (marker <= kPositiveFixMax) {
      return marker;
    }
    if (marker >= kNegativeFixMin) {
      return static_cast<std::int8_t>(marker);
    }
    switch (marker) {
      case kUint8:
        return static_cast<std::int64_t>(big_endian(kBytes1));
      case kUint16:
        return static_cast<std::int64_t>(big_endian(kBytes2));
      case kUint32:
        return static_cast<std::int64_t>(big_endian(kBytes4));
      case kUint64: {
        const std::uint64_t value = big_endian(kBytes8);
        if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
          fail("an integer is too large");
        }
        return static_cast<std::int64_t>(value);
      }
      case kInt8:
        return static_cast<std::int8_t>(big_endian(kBytes1));
      case kInt16:
        return static_cast<std::int16_t>(big_endian(kBytes2));
      case kInt32:
        return static_cast<std::int32_t>(big_endian(kBytes4));
      case kInt64:
        return static_cast<std::int64_t>(big_endian(kBytes8));
      default:
        fail("expected an integer");
    }
  }

  // A non-negative integer, up to the largest uint64.
  std::uint64_t unsigned_integer() {
    if (peek_byte() == kUint64) {
      ++position_;
      return big_endian(kBytes8);
    }
    const std::int64_t value = integer();
    if (value < 0) {
      fail("expected a non-negative integer");
    }
    return static_cast<std::uint64_t>(value);
  }

  // An unsigned integer that must fit in `limit`.
  std::uint64_t bounded(std::uint64_t limit, std::string_view what) {
    const std::uint64_t value = unsigned_integer();
    if (value > limit) {
      fail(std::string{what} + " is out of range");
    }
    return value;
  }

  double number() {
    const std::uint8_t marker = next();
    if (marker == kFloat64) {
      return std::bit_cast<double>(big_endian(kBytes8));
    }
    if (marker == kFloat32) {
      return static_cast<double>(
          std::bit_cast<float>(static_cast<std::uint32_t>(big_endian(kBytes4))));
    }
    fail("expected a float");
  }

  std::string str() {
    const std::uint8_t marker = next();
    std::size_t size = 0;
    if ((marker & kFixStrKindMask) == kFixStrBase) {
      size = marker & kFixStrCountMask;
    } else if (marker == kStr8) {
      size = big_endian(kBytes1);
    } else if (marker == kStr16) {
      size = big_endian(kBytes2);
    } else if (marker == kStr32) {
      size = big_endian(kBytes4);
    } else {
      fail("expected a string");
    }
    std::string text;
    for (const std::byte b : take(size)) {
      text.push_back(std::to_integer<char>(b));
    }
    return text;
  }

  // A view into the data.
  std::span<const std::byte> bin() {
    const std::uint8_t marker = next();
    std::size_t size = 0;
    if (marker == kBin8) {
      size = big_endian(kBytes1);
    } else if (marker == kBin16) {
      size = big_endian(kBytes2);
    } else if (marker == kBin32) {
      size = big_endian(kBytes4);
    } else {
      fail("expected binary data");
    }
    return take(size);
  }

  // The element count of an array. Every element takes a byte at least, so a count over the
  // bytes left is false, and nothing can reserve more memory than the file has bytes.
  std::size_t array() { return container(kFixArrayBase, kArray16, kArray32, 1); }
  // The pair count of a map (two bytes at least for each).
  std::size_t map() { return container(kFixMapBase, kMap16, kMap32, 2); }

  void skip(int depth) {
    if (depth > kMaxSkipDepth) {
      fail("data nested too deep");
    }
    const Kind kind = peek_kind();
    if (kind == Kind::kArray || kind == Kind::kMap) {
      const std::size_t count = kind == Kind::kArray ? array() : map() * 2;
      for (std::size_t i = 0; i < count; ++i) {
        skip(depth + 1);
      }
    } else if (kind == Kind::kString) {
      static_cast<void>(str());
    } else if (kind == Kind::kBinary) {
      static_cast<void>(bin());
    } else if (kind == Kind::kInteger) {
      static_cast<void>(unsigned_integer_or_negative());
    } else if (kind == Kind::kFloat) {
      static_cast<void>(number());
    } else if (kind == Kind::kBool) {
      static_cast<void>(boolean());
    } else {
      fail("unsupported MessagePack type");
    }
  }

 private:
  // Skips an integer of either sign (uint64 above the int64 range included).
  std::int64_t unsigned_integer_or_negative() {
    if (peek_byte() == kUint64) {
      ++position_;
      return static_cast<std::int64_t>(big_endian(kBytes8));
    }
    return integer();
  }

  std::size_t container(std::uint8_t fix_base, std::uint8_t marker16, std::uint8_t marker32,
                        std::size_t bytes_per_item) {
    const std::uint8_t marker = next();
    std::size_t count = 0;
    if ((marker & kFixKindMask) == fix_base) {
      count = marker & kFixCountMask;
    } else if (marker == marker16) {
      count = big_endian(kBytes2);
    } else if (marker == marker32) {
      count = big_endian(kBytes4);
    } else {
      fail(fix_base == kFixMapBase ? "expected a map" : "expected an array");
    }
    if (count > remaining() / bytes_per_item) {
      fail("the data ends early");
    }
    return count;
  }

  void need(std::size_t count) const {
    if (count > remaining()) {
      fail("the data ends early");
    }
  }

  [[nodiscard]] std::uint8_t peek_byte() const {
    need(1);
    return std::to_integer<std::uint8_t>(data_.subspan(position_).front());
  }

  std::uint8_t next() {
    const std::uint8_t value = peek_byte();
    ++position_;
    return value;
  }

  std::span<const std::byte> take(std::size_t count) {
    need(count);
    const std::span<const std::byte> taken = data_.subspan(position_, count);
    position_ += count;
    return taken;
  }

  std::uint64_t big_endian(std::size_t size) {
    std::uint64_t value = 0;
    for (const std::byte b : take(size)) {
      value = (value << kBitsPerByte) | std::to_integer<std::uint64_t>(b);
    }
    return value;
  }

  std::span<const std::byte> data_;
  std::size_t position_ = 0;
};

// ---- The save payload ----------------------------------------------------------------------

enum class Key : std::uint8_t {
  kFormat,
  kEngine,
  kGame,
  kLabel,
  kStep,
  kHash,
  kStore,
  kScene,
  kInputs,
  kPersistent,
  kThumbnail,
  kTime,
  kTitle,
  kCount,
};

constexpr std::size_t kKeyCount = static_cast<std::size_t>(Key::kCount);

constexpr std::array<std::string_view, kKeyCount> kKeyNames{
    "format", "engine", "game",       "label",     "step", "hash", "store",
    "scene",  "inputs", "persistent", "thumbnail", "time", "title"};

constexpr std::size_t kThumbnailChannels = 4;
constexpr std::size_t kThumbnailFields = 3;
constexpr std::size_t kInputFields = 2;
constexpr std::uint64_t kMenuChoiceTag = 0;
constexpr std::uint64_t kScreenResultTag = 1;
constexpr std::uint64_t kStoreWriteTag = 2;
constexpr std::size_t kStoreWriteFields = 3;

std::string_view key_name(Key key) { return kKeyNames.at(static_cast<std::size_t>(key)); }

void write_value(Writer& out, const StoreValue& value) {
  if (const auto* boolean = std::get_if<bool>(&value)) {
    out.boolean(*boolean);
  } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    out.integer(*integer);
  } else if (const auto* number = std::get_if<double>(&value)) {
    out.number(*number);
  } else if (const auto* text = std::get_if<std::string>(&value)) {
    out.str(*text);
  } else {
    out.array(1);
    out.unsigned_integer(std::get<StoreTableRef>(value).index);
  }
}

StoreValue read_value(Reader& in, bool allow_table) {
  switch (in.peek_kind()) {
    case Kind::kBool:
      return in.boolean();
    case Kind::kInteger:
      return in.integer();
    case Kind::kFloat:
      return in.number();
    case Kind::kString:
      return in.str();
    case Kind::kArray:
      if (allow_table && in.array() == 1) {
        return StoreTableRef{.index = static_cast<std::uint32_t>(in.bounded(
                                 std::numeric_limits<std::uint32_t>::max(), "a table reference"))};
      }
      fail("a store key or value is not a boolean, number, string or table");
    default:
      fail("a store key or value is not a boolean, number, string or table");
  }
}

void write_screen_value(Writer& out, const ScreenValue& value) {
  if (const auto* boolean = std::get_if<bool>(&value)) {
    out.boolean(*boolean);
  } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
    out.integer(*integer);
  } else if (const auto* number = std::get_if<double>(&value)) {
    out.number(*number);
  } else if (const auto* text = std::get_if<std::string>(&value)) {
    out.str(*text);
  } else {
    out.nil();
  }
}

ScreenValue read_screen_value(Reader& in) {
  if (in.peek_nil()) {
    in.nil();
    return {};
  }
  switch (in.peek_kind()) {
    case Kind::kBool:
      return in.boolean();
    case Kind::kInteger:
      return in.integer();
    case Kind::kFloat:
      return in.number();
    case Kind::kString:
      return in.str();
    default:
      fail("a screen result is not nil, a boolean, a number or a string");
  }
}

void write_store(Writer& out, const StoreSnapshot& store) {
  out.array(store.tables.size());
  for (const StoreTable& table : store.tables) {
    out.array(table.entries.size() * 2);
    for (const auto& [key, value] : table.entries) {
      write_value(out, key);
      write_value(out, value);
    }
  }
}

StoreSnapshot read_store(Reader& in) {
  StoreSnapshot store;
  const std::size_t table_count = in.array();
  if (table_count == 0) {
    fail("the store snapshot has no tables");
  }
  store.tables.reserve(table_count);
  for (std::size_t t = 0; t < table_count; ++t) {
    const std::size_t item_count = in.array();
    if (item_count % 2 != 0) {
      fail("a store table has a key without a value");
    }
    StoreTable table;
    table.entries.reserve(item_count / 2);
    for (std::size_t i = 0; i < item_count; i += 2) {
      StoreValue key = read_value(in, false);
      if (const auto* number = std::get_if<double>(&key);
          number != nullptr && std::isnan(*number)) {
        fail("a store key is NaN");
      }
      StoreValue value = read_value(in, true);
      table.entries.emplace_back(std::move(key), std::move(value));
    }
    store.tables.push_back(std::move(table));
  }
  for (const StoreTable& table : store.tables) {
    for (const auto& entry : table.entries) {
      const auto* ref = std::get_if<StoreTableRef>(&entry.second);
      if (ref != nullptr && ref->index >= table_count) {
        fail("a store value refers to a table that does not exist");
      }
    }
  }
  return store;
}

std::vector<InputResult> read_inputs(Reader& in) {
  std::vector<InputResult> inputs;
  const std::size_t count = in.array();
  inputs.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    if (in.array() != kInputFields) {
      fail("an input log entry has the wrong size");
    }
    const std::uint64_t tag = in.unsigned_integer();
    if (tag == kMenuChoiceTag) {
      inputs.emplace_back(
          MenuChoice{.index = static_cast<std::size_t>(
                         in.bounded(std::numeric_limits<std::uint32_t>::max(), "a menu choice"))});
    } else if (tag == kScreenResultTag) {
      inputs.emplace_back(ScreenResult{.value = read_screen_value(in)});
    } else if (tag == kStoreWriteTag) {
      if (in.array() != kStoreWriteFields) {
        fail("a store write has the wrong size");
      }
      StoreWrite write;
      write.wait = static_cast<std::uint32_t>(
          in.bounded(std::numeric_limits<std::uint32_t>::max(), "a store write wait"));
      write.key = in.str();
      write.value = read_screen_value(in);
      inputs.emplace_back(std::move(write));
    } else {
      fail("an input log entry has an unknown kind");
    }
  }
  return inputs;
}

Thumbnail read_thumbnail(Reader& in) {
  if (in.array() != kThumbnailFields) {
    fail("the thumbnail has the wrong size");
  }
  Thumbnail thumbnail;
  thumbnail.width = static_cast<int>(in.bounded(kMaxThumbnailSide, "the thumbnail width"));
  thumbnail.height = static_cast<int>(in.bounded(kMaxThumbnailSide, "the thumbnail height"));
  const std::span<const std::byte> pixels = in.bin();
  const auto expected = static_cast<std::size_t>(thumbnail.width) *
                        static_cast<std::size_t>(thumbnail.height) * kThumbnailChannels;
  if (pixels.size() != expected) {
    fail("the thumbnail has the wrong number of bytes");
  }
  thumbnail.rgba.reserve(pixels.size());
  for (const std::byte b : pixels) {
    thumbnail.rgba.push_back(std::to_integer<std::uint8_t>(b));
  }
  return thumbnail;
}

constexpr std::size_t kPersistentFieldCount = 3;
constexpr std::size_t kSeenFields = 3;
constexpr std::string_view kPersistentFormatKey = "format";
constexpr std::string_view kPersistentStoreKey = "store";
constexpr std::string_view kPersistentSeenKey = "seen";

constexpr int kCompressionLevel = 3;

}  // namespace

std::vector<std::byte> encode_save_payload(const SaveData& data) {
  Writer out;
  out.map(kKeyCount);
  out.str(key_name(Key::kFormat));
  out.unsigned_integer(data.format);
  out.str(key_name(Key::kEngine));
  out.str(data.engine_version);
  out.str(key_name(Key::kGame));
  out.str(data.game_version);
  out.str(key_name(Key::kLabel));
  out.str(data.label);
  out.str(key_name(Key::kStep));
  out.unsigned_integer(data.step);
  out.str(key_name(Key::kHash));
  out.unsigned_integer(data.source_hash);

  out.str(key_name(Key::kStore));
  write_store(out, data.snapshot.store);
  out.str(key_name(Key::kScene));
  out.bin(data.snapshot.scene);

  out.str(key_name(Key::kInputs));
  out.array(data.inputs.size());
  for (const InputResult& input : data.inputs) {
    out.array(kInputFields);
    if (const auto* choice = std::get_if<MenuChoice>(&input)) {
      out.unsigned_integer(kMenuChoiceTag);
      out.unsigned_integer(choice->index);
    } else if (const auto* result = std::get_if<ScreenResult>(&input)) {
      out.unsigned_integer(kScreenResultTag);
      write_screen_value(out, result->value);
    } else {
      const auto& write = std::get<StoreWrite>(input);
      out.unsigned_integer(kStoreWriteTag);
      out.array(kStoreWriteFields);
      out.unsigned_integer(write.wait);
      out.str(write.key);
      write_screen_value(out, write.value);
    }
  }
  out.str(key_name(Key::kPersistent));
  out.str(data.persistent);

  out.str(key_name(Key::kThumbnail));
  out.array(kThumbnailFields);
  out.unsigned_integer(static_cast<std::uint64_t>(data.thumbnail.width));
  out.unsigned_integer(static_cast<std::uint64_t>(data.thumbnail.height));
  out.bin(std::as_bytes(std::span<const std::uint8_t>{data.thumbnail.rgba}));

  out.str(key_name(Key::kTime));
  out.integer(data.time);
  out.str(key_name(Key::kTitle));
  out.str(data.title);
  return out.take();
}

SaveData decode_save_payload(std::span<const std::byte> payload) {
  Reader in{payload};
  SaveData data;
  std::array<bool, kKeyCount> seen{};
  const std::size_t entries = in.map();
  for (std::size_t i = 0; i < entries; ++i) {
    const std::string name = in.str();
    std::size_t index = 0;
    while (index < kKeyCount && kKeyNames.at(index) != name) {
      ++index;
    }
    if (index == kKeyCount) {
      in.skip(0);
      continue;
    }
    if (seen.at(index)) {
      fail("the key '" + name + "' is repeated");
    }
    seen.at(index) = true;
    switch (static_cast<Key>(index)) {
      case Key::kFormat:
        if (in.unsigned_integer() != kSaveFormatVersion) {
          fail("made by another save format");
        }
        break;
      case Key::kEngine:
        data.engine_version = in.str();
        break;
      case Key::kGame:
        data.game_version = in.str();
        break;
      case Key::kLabel:
        data.label = in.str();
        break;
      case Key::kStep:
        data.step = static_cast<std::uint32_t>(
            in.bounded(std::numeric_limits<std::uint32_t>::max(), "the step"));
        break;
      case Key::kHash:
        data.source_hash = in.unsigned_integer();
        break;
      case Key::kStore:
        data.snapshot.store = read_store(in);
        break;
      case Key::kScene: {
        const std::span<const std::byte> scene = in.bin();
        data.snapshot.scene.assign(scene.begin(), scene.end());
        break;
      }
      case Key::kInputs:
        data.inputs = read_inputs(in);
        break;
      case Key::kPersistent:
        data.persistent = in.str();
        break;
      case Key::kThumbnail:
        data.thumbnail = read_thumbnail(in);
        break;
      case Key::kTime:
        data.time = in.integer();
        break;
      case Key::kTitle:
        data.title = in.str();
        break;
      case Key::kCount:
        break;
    }
  }
  if (!in.at_end()) {
    fail("unexpected data after the last key");
  }
  for (std::size_t key = 0; key < kKeyCount; ++key) {
    if (!seen.at(key)) {
      fail("the key '" + std::string{kKeyNames.at(key)} + "' is missing");
    }
  }
  data.snapshot.label = data.label;
  return data;
}

std::vector<std::byte> encode_persistent(const PersistentData& data) {
  Writer out;
  out.map(kPersistentFieldCount);
  out.str(kPersistentFormatKey);
  out.unsigned_integer(kPersistentFormatVersion);
  out.str(kPersistentStoreKey);
  write_store(out, data.store);
  out.str(kPersistentSeenKey);
  out.array(data.seen.size());
  for (const SeenStep& step : data.seen) {
    out.array(kSeenFields);
    out.str(step.label);
    out.unsigned_integer(step.step);
    out.unsigned_integer(step.hash);
  }
  return out.take();
}

PersistentData decode_persistent(std::span<const std::byte> bytes) {
  Reader in{bytes};
  PersistentData data;
  bool have_store = false;
  const std::size_t entries = in.map();
  for (std::size_t i = 0; i < entries; ++i) {
    const std::string name = in.str();
    if (name == kPersistentFormatKey) {
      if (in.unsigned_integer() != kPersistentFormatVersion) {
        fail("persistent: made by another format");
      }
    } else if (name == kPersistentStoreKey) {
      data.store = read_store(in);
      have_store = true;
    } else if (name == kPersistentSeenKey) {
      const std::size_t count = in.array();
      data.seen.reserve(count);
      for (std::size_t n = 0; n < count; ++n) {
        if (in.array() != kSeenFields) {
          fail("a seen step has the wrong size");
        }
        SeenStep step;
        step.label = in.str();
        step.step = static_cast<std::uint32_t>(
            in.bounded(std::numeric_limits<std::uint32_t>::max(), "a seen step"));
        step.hash = in.unsigned_integer();
        data.seen.push_back(std::move(step));
      }
    } else {
      in.skip(0);
    }
  }
  if (!in.at_end()) {
    fail("unexpected data after the last key");
  }
  if (!have_store) {
    fail("the persistent store is missing");
  }
  return data;
}

std::vector<std::byte> encode_save(const SaveData& data) {
  const std::vector<std::byte> payload = encode_save_payload(data);
  std::vector<std::byte> file(ZSTD_compressBound(payload.size()));
  const std::size_t size =
      ZSTD_compress(file.data(), file.size(), payload.data(), payload.size(), kCompressionLevel);
  if (ZSTD_isError(size) != 0) {
    throw SaveError{std::string{"save: compression failed: "} + ZSTD_getErrorName(size)};
  }
  file.resize(size);
  return file;
}

SaveData decode_save(std::span<const std::byte> file) {
  const unsigned long long size = ZSTD_getFrameContentSize(file.data(), file.size());
  if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN) {
    fail("not a save file");
  }
  if (size > kMaxSavePayloadBytes) {
    fail("the file is too large");
  }
  std::vector<std::byte> payload(static_cast<std::size_t>(size));
  const std::size_t unpacked =
      ZSTD_decompress(payload.data(), payload.size(), file.data(), file.size());
  if (ZSTD_isError(unpacked) != 0 || unpacked != payload.size()) {
    fail("the file is damaged");
  }
  return decode_save_payload(payload);
}

}  // namespace vnmaki
