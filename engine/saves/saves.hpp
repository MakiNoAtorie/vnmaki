#pragma once

// Save slots, quick save and auto save (docs/SPEC.md section 8). The file format is in
// save_file.hpp.
//
// A save is made while the story waits (RunState::kWaiting) and records the label, the step, the
// label-entry snapshot and the input log. A load runs the script's `on_load` hook on the saved
// store, then restores the position by ScriptRuntime::replay(), so the story waits at the saved
// line again.
//
// Saves live in a directory the host gives (user_data_directory() in
// engine/platform/sdl_platform.hpp, or a temporary directory in tests), one file per slot:
// `<slot>.vnsave`. They are not in the Vfs. A slot name is 1 to 32 characters of a-z, 0-9, `_`
// and `-`; scripts use numbers, "quick" and "auto".

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/runtime/project_config.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/save_file.hpp"

namespace vnmaki {

inline constexpr std::string_view kQuickSlot = "quick";
inline constexpr std::string_view kAutoSlot = "auto";
/// The extension of a save file.
inline constexpr std::string_view kSaveExtension = ".vnsave";
/// The longest title written to a save, in bytes.
inline constexpr std::size_t kMaxSaveTitleBytes = 80;

struct SavesOptions {
  /// Where the slot files are. Created at the first save.
  std::filesystem::path directory{};
  /// Written to every save; defaults to the engine's version.
  std::string engine_version{};
  /// Written to every save, and given to `on_load` as `old_version` when it is read again
  /// (project.toml `version`).
  std::string game_version{"0.0.0"};
  /// Written to every save as the place of the persistent store.
  std::string persistent{"persistent"};
  /// Events that write the auto-save slot (project.toml `auto_save`).
  std::vector<AutoSaveEvent> auto_save{};
  /// Makes the thumbnail of a save, e.g. capture_thumbnail() on the screen target. Without it
  /// saves get stub_thumbnail().
  std::function<Thumbnail()> thumbnail{};
  /// Unix time in seconds. Without it the system clock.
  std::function<std::int64_t()> clock{};
  /// Receives dev warnings, and the errors of saves and loads that scripts or auto saves started
  /// (they have no caller to return them to).
  std::function<void(std::string_view)> on_warning{};
};

/// What the slot list shows about one save file.
struct SlotInfo {
  std::string slot{};
  /// Why the file cannot be read; empty if it can. The other fields are then empty.
  std::string error{};
  std::string title{};
  std::string label{};
  std::string game_version{};
  std::int64_t time = 0;
  Thumbnail thumbnail{};
};

/// Saves and loads one ScriptRuntime. Use it from the thread that drives the runtime. The runtime
/// must outlive it. Functions that can fail return why, or an empty string.
class Saves {
 public:
  Saves(ScriptRuntime& runtime, SavesOptions options);

  /// Writes the story's current position to `slot`, replacing what is there. Fails unless the
  /// story waits at a line or menu.
  [[nodiscard]] std::string save(std::string_view slot);
  /// Restores the position saved in `slot`. If the label's source changed since, the story
  /// starts that label from its first line and on_warning is called. If a load fails the story
  /// goes back to where it was.
  [[nodiscard]] std::string load(std::string_view slot);
  [[nodiscard]] std::string quick_save() { return save(kQuickSlot); }
  [[nodiscard]] std::string quick_load() { return load(kQuickSlot); }
  [[nodiscard]] std::string auto_save() { return save(kAutoSlot); }
  /// Deletes the file of `slot`. A slot that is empty is not an error.
  [[nodiscard]] std::string remove(std::string_view slot);

  [[nodiscard]] bool exists(std::string_view slot) const;
  /// Every slot file, numbered slots first in number order, then the rest by name. A file that
  /// cannot be read is listed with its error.
  [[nodiscard]] std::vector<SlotInfo> list() const;

  /// Call it whenever the story waits (after start, advance, choose and load; calling it more
  /// often is harmless). It carries out the save and load requests of scripts, then writes the
  /// auto-save slot if the story just reached an event of SavesOptions::auto_save.
  void poll();

 private:
  /// Fills `data` from the runtime; `full` also takes the thumbnail and the time.
  [[nodiscard]] std::string capture(SaveData& data, bool full) const;
  [[nodiscard]] std::filesystem::path path_of(std::string_view slot) const;
  void warn(std::string_view message) const;
  [[nodiscard]] std::string write_file(std::string_view slot, const SaveData& data) const;
  void auto_save_if_due();

  ScriptRuntime* runtime_;
  SavesOptions options_;
  /// Where the story was when poll() last looked; an event fires once per position.
  std::optional<ScriptPosition> last_position_{};
};

}  // namespace vnmaki
