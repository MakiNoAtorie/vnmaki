#pragma once

// The save file format (docs/SPEC.md section 8): MessagePack, compressed with zstd.
//
// The payload is a MessagePack map with these keys, in this order when written (a reader accepts
// any order and skips keys it does not know):
//   format      uint     kSaveFormatVersion
//   engine      str      engine version that wrote the file
//   game        str      game version (project.toml `version`) that wrote the file
//   label       str      the label the story waited in
//   step        uint     yield points that label had reached
//   hash        uint     ScriptRuntime::label_source_hash of that label
//   store       array    the store of the label-entry snapshot: one array per table, each
//                        [key, value, key, value, ...]; a value that is a table is [index]
//   scene       bin      the scene snapshot of the label-entry snapshot
//   inputs      array    the input log: [0, choice] for a menu choice, [1, value] for the result
//                        of a call_screen (value: nil, bool, int, float or str)
//   persistent  str      where the persistent store lives (it is not copied into the save)
//   thumbnail   array    [width, height, rgba bytes]
//   time        int      Unix time of the save, in seconds
//   title       str      text for the slot list
//
// Nothing in a file is trusted: decode_save() rejects bad data with SaveError and never reads
// outside its input.

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

/// The `format` of files this engine writes. A file with another value is rejected.
inline constexpr std::uint32_t kSaveFormatVersion = 1;
/// The longest side of a thumbnail, in pixels.
inline constexpr int kMaxThumbnailSide = 512;
/// The largest payload decode_save() unpacks (a hostile file cannot claim more memory).
inline constexpr std::size_t kMaxSavePayloadBytes = std::size_t{32} * 1024 * 1024;

/// A preview of the screen: 8-bit RGBA with premultiplied alpha, rows from top to bottom, as
/// Image. Empty (0x0) when there is none.
struct Thumbnail {
  int width = 0;
  int height = 0;
  /// width * height * 4 bytes.
  std::vector<std::uint8_t> rgba{};

  friend bool operator==(const Thumbnail&, const Thumbnail&) = default;
};

/// What a save file holds.
struct SaveData {
  std::uint32_t format = kSaveFormatVersion;
  std::string engine_version{};
  std::string game_version{};
  /// The label the story waited in; `snapshot.label` is the same.
  std::string label{};
  std::uint32_t step = 0;
  std::uint64_t source_hash = 0;
  /// The label-entry snapshot of `label`.
  LabelSnapshot snapshot{};
  std::vector<InputResult> inputs{};
  std::string persistent{};
  Thumbnail thumbnail{};
  std::int64_t time = 0;
  std::string title{};

  friend bool operator==(const SaveData&, const SaveData&) = default;
};

/// A save file is damaged, made by another format, or not a save at all. The message says why.
class SaveError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// The MessagePack payload of `data`. The same data always gives the same bytes.
[[nodiscard]] std::vector<std::byte> encode_save_payload(const SaveData& data);
/// Reads a payload. Throws SaveError. It checks what replay needs: `format`, the store snapshot
/// (table references, keys) and the thumbnail size.
[[nodiscard]] SaveData decode_save_payload(std::span<const std::byte> payload);

/// One line of text the player has seen: the label, the step in it and the source hash of the
/// label. A line that is edited gets another hash, so it is unseen again.
struct SeenStep {
  std::string label{};
  std::uint32_t step = 0;
  std::uint64_t hash = 0;

  friend auto operator<=>(const SeenStep&, const SeenStep&) = default;
};

/// What the persistent file holds: the `persistent` table and the seen steps.
struct PersistentData {
  StoreSnapshot store{};
  std::vector<SeenStep> seen{};
};

/// The `format` of the persistent file.
inline constexpr std::uint32_t kPersistentFormatVersion = 1;

/// MessagePack, not compressed: a map with `format`, `store` (as in a save) and `seen`
/// ([label, step, hash] for each step).
[[nodiscard]] std::vector<std::byte> encode_persistent(const PersistentData& data);
/// Throws SaveError. Like decode_save(), it never trusts its input.
[[nodiscard]] PersistentData decode_persistent(std::span<const std::byte> bytes);

/// The file form: the payload compressed with zstd.
[[nodiscard]] std::vector<std::byte> encode_save(const SaveData& data);
/// Throws SaveError.
[[nodiscard]] SaveData decode_save(std::span<const std::byte> file);

}  // namespace vnmaki
