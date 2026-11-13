#pragma once

// The persistent store (docs/SPEC.md section 8): the script's `persistent` table and the set of
// text the player has seen, kept in one file in the user-data directory. Rollback and load never
// change it: it is not part of a snapshot or a save.

#include <filesystem>
#include <functional>
#include <set>
#include <string_view>

#include "engine/runtime/script_runtime.hpp"
#include "engine/saves/save_file.hpp"

namespace vnmaki {

/// The name of the persistent file inside the directory.
inline constexpr std::string_view kPersistentFileName = "persistent.vnpersist";

/// Loads the file into one ScriptRuntime and writes it back when `persistent` or the seen set
/// changed. The runtime must outlive it. Use it from the thread that drives the runtime.
class PersistentStore {
 public:
  /// `directory` is where the file is (the same as the saves); it is created at the first write.
  /// `on_warning` receives the problems of reading and writing; none stops the game.
  PersistentStore(ScriptRuntime& runtime, std::filesystem::path directory,
                  std::function<void(std::string_view)> on_warning = {});
  PersistentStore(const PersistentStore&) = delete;
  PersistentStore(PersistentStore&&) = delete;
  PersistentStore& operator=(const PersistentStore&) = delete;
  PersistentStore& operator=(PersistentStore&&) = delete;
  /// Writes the changes (see flush()), so the data is saved on exit.
  ~PersistentStore();

  /// Reads the file, if there is one, into `persistent` and the seen set. Call it before
  /// ScriptRuntime::load_scripts(), so scripts that set defaults at their top level see the saved
  /// values. A file that cannot be read is reported and ignored: the game starts with no data.
  void load();

  /// Writes the file if `persistent` or the seen set changed since the last load() or flush().
  /// Call it whenever the story waits; it costs one copy of the table.
  void flush();

  /// Whether the player has seen the line at `position` of this version of its label.
  [[nodiscard]] bool seen(const ScriptPosition& position) const;
  /// Adds the line at `position` to the seen set.
  void mark_seen(const ScriptPosition& position);

  [[nodiscard]] std::size_t seen_count() const { return seen_.size(); }

 private:
  void warn(std::string_view message) const;
  [[nodiscard]] SeenStep key_of(const ScriptPosition& position) const;

  ScriptRuntime* runtime_;
  std::filesystem::path file_;
  std::function<void(std::string_view)> on_warning_;
  std::set<SeenStep> seen_;
  StoreSnapshot written_{};
  bool seen_dirty_ = false;
  bool warned_ = false;
};

}  // namespace vnmaki
