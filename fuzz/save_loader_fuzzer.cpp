// Fuzzes the save loader (engine/saves/save_file.hpp): save files are untrusted input.
//
// Random bytes rarely make a valid save, so the input is used in two ways:
//   - as a file or payload as it is (the loader must reject it), and
//   - as a list of byte edits to a valid payload (3 bytes each: position, new value), plus a
//     truncation, so the loader reaches its deep checks.
// Besides "does not crash, throws only SaveError", a save the loader accepts must keep what the
// loader promises (every table reference exists, the thumbnail has width * height * 4 bytes) and
// must survive encoding: the bytes of the encoded save decode to a save that encodes to the
// same bytes. (Bytes, not SaveData, are compared because a NaN store value is never equal to
// itself.)

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "engine/saves/save_file.hpp"

namespace {

constexpr std::size_t kChannels = 4;
constexpr std::size_t kEditSize = 3;
constexpr unsigned kByteBits = 8;
constexpr double kSampleNumber = 2.5;
constexpr std::int64_t kSampleInteger = -300;
constexpr std::uint64_t kSampleHash = 0x0123456789ABCDEFULL;
constexpr std::int64_t kSampleTime = 1'700'000'000;
constexpr std::size_t kSampleThumbnailSide = 2;
constexpr std::size_t kSampleThumbnailBytes =
    kSampleThumbnailSide * kSampleThumbnailSide * kChannels;
constexpr std::uint8_t kSamplePixel = 0x7F;

vnmaki::SaveData sample() {
  vnmaki::SaveData data;
  data.engine_version = "0.1.0";
  data.game_version = "1.0.0";
  data.label = "start";
  data.step = 3;
  data.source_hash = kSampleHash;
  data.snapshot.label = data.label;
  data.snapshot.store.tables = {
      vnmaki::StoreTable{
          .entries = {{std::string{"flag"}, true},
                      {std::string{"count"}, kSampleInteger},
                      {std::string{"ratio"}, kSampleNumber},
                      {std::string{"name"}, std::string{"Eileen"}},
                      {std::string{"inventory"}, vnmaki::StoreTableRef{.index = 1}}}},
      vnmaki::StoreTable{.entries = {{std::int64_t{1}, std::string{"key"}},
                                     {std::string{"self"}, vnmaki::StoreTableRef{.index = 1}}}}};
  data.snapshot.scene = {std::byte{1}, std::byte{2}, std::byte{3}};
  data.inputs = {vnmaki::MenuChoice{.index = 1}, vnmaki::MenuChoice{.index = 0}};
  data.persistent = "persistent";
  data.thumbnail = {.width = static_cast<int>(kSampleThumbnailSide),
                    .height = static_cast<int>(kSampleThumbnailSide),
                    .rgba = std::vector<std::uint8_t>(kSampleThumbnailBytes, kSamplePixel)};
  data.time = kSampleTime;
  data.title = "You came back.";
  return data;
}

void check_promises(const vnmaki::SaveData& data) {
  const std::size_t tables = data.snapshot.store.tables.size();
  if (tables == 0) {
    std::abort();
  }
  for (const vnmaki::StoreTable& table : data.snapshot.store.tables) {
    for (const auto& [key, value] : table.entries) {
      const auto* ref = std::get_if<vnmaki::StoreTableRef>(&value);
      if ((ref != nullptr && ref->index >= tables) ||
          std::holds_alternative<vnmaki::StoreTableRef>(key)) {
        std::abort();
      }
    }
  }
  const auto pixels = static_cast<std::size_t>(data.thumbnail.width) *
                      static_cast<std::size_t>(data.thumbnail.height) * kChannels;
  if (data.thumbnail.rgba.size() != pixels || data.snapshot.label != data.label) {
    std::abort();
  }
}

void check_round_trip(const vnmaki::SaveData& data) {
  check_promises(data);
  const std::vector<std::byte> payload = vnmaki::encode_save_payload(data);
  if (vnmaki::encode_save_payload(vnmaki::decode_save_payload(payload)) != payload) {
    std::abort();
  }
  if (vnmaki::encode_save_payload(vnmaki::decode_save(vnmaki::encode_save(data))) != payload) {
    std::abort();
  }
}

// Decodes with `decode`; a rejected input is the expected outcome and checks nothing more.
template <typename Decode>
void check(Decode decode) {
  std::optional<vnmaki::SaveData> accepted;
  try {
    accepted = decode();
  } catch (const vnmaki::SaveError&) {
    return;
  }
  check_round_trip(*accepted);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::span<const std::uint8_t> edits{data, size};
  const std::span<const std::byte> input = std::as_bytes(edits);
  check([&] { return vnmaki::decode_save_payload(input); });
  check([&] { return vnmaki::decode_save(input); });

  std::vector<std::byte> payload = vnmaki::encode_save_payload(sample());
  for (std::size_t i = 0; i + kEditSize <= size; i += kEditSize) {
    const std::span<const std::uint8_t> edit = edits.subspan(i, kEditSize);
    const std::size_t position =
        ((static_cast<std::size_t>(edit.front()) << kByteBits) | edit.subspan(1).front()) %
        payload.size();
    payload.at(position) = static_cast<std::byte>(edit.back());
  }
  if (size != 0 && size % 2 == 0) {
    payload.resize(payload.size() - edits.front() % payload.size());
  }
  check([&] { return vnmaki::decode_save_payload(payload); });
  return 0;
}
