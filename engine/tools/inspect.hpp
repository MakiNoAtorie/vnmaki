#pragma once

// The data of the dev overlay's inspector panels (docs/SPEC.md section 18). These functions
// return rows and use no ImGui, so tests can check them; the overlay only draws the rows.

#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/audio.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"

namespace vnmaki {

/// One line of a table tree. A table's row comes first, then the rows of its fields at
/// `depth + 1`. `value` is empty for a table that is expanded below its row.
struct StoreRow {
  int depth = 0;
  std::string key{};
  std::string value{};
  bool is_table = false;
};

/// `store` as rows, in snapshot order. A table that two fields share is expanded once; the
/// other field says so in `value`.
[[nodiscard]] std::vector<StoreRow> inspect_store(const ScriptRuntime& runtime);
/// `persistent` as rows (it holds `prefs`, the seen text and the unlocks).
[[nodiscard]] std::vector<StoreRow> inspect_persistent(const ScriptRuntime& runtime);

struct DisplayableRow {
  std::string tag{};
  std::string kind{};  ///< "image <path>", "layered", "animation", "movie" or "displayable".
};

struct LayerRows {
  std::string layer{};
  std::vector<DisplayableRow> displayables{};  ///< Back to front.
};

/// Every layer, back to front, also the empty ones.
[[nodiscard]] std::vector<LayerRows> inspect_scene(const Scene& scene);

struct PropertyRow {
  std::string name{};  ///< x, y, anchor_x, scale_x, rotation, alpha, tint_r, crop_x, uniform_0...
  double value = 0;
};

/// The properties of the displayable `tag` (virtual pixels, fractions, degrees); empty when
/// no such displayable is shown. The `uniform_n` rows exist when it has a game shader.
[[nodiscard]] std::vector<PropertyRow> inspect_properties(const Scene& scene,
                                                          std::string_view tag);

/// Sets one property at once, from the next frame on. Alpha and tint stay in [0, 1]. The edit
/// is not in any snapshot: it stays until a rollback, a load or a tween writes the property.
/// Returns why it could not be done, or an empty string.
[[nodiscard]] std::string set_property(Scene& scene, std::string_view tag, std::string_view name,
                                       double value);

struct TweenRow {
  std::string tag{};
  std::string channels{};  ///< The property names, separated by ", ".
  std::string ease{};
  double elapsed = 0;
  double duration = 0;
  bool loop = false;
};

[[nodiscard]] std::vector<TweenRow> inspect_tweens(const Scene& scene);

struct AudioRow {
  std::string channel{};
  std::string file{};   ///< Empty when nothing plays on the channel.
  double volume = 1;    ///< The channel volume (the script's `volume`).
  double position = 0;  ///< Seconds into the file.
  bool loop = false;
};

/// One row for each playing track, and one for a channel with none.
[[nodiscard]] std::vector<AudioRow> inspect_audio(const Audio& audio);

struct LabelRow {
  std::string name{};
  bool current = false;  ///< The story waits in this label.
};

/// All labels, sorted by name.
[[nodiscard]] std::vector<LabelRow> inspect_labels(const ScriptRuntime& runtime);

/// "Jump": starts `label` at step 1 through ScriptRuntime::replay(), with the current `store`
/// and the scene of the running label's entry, no callers and no inputs. If the label cannot
/// reach step 1 (it has no blocking call), the story goes back to where it was. Returns the
/// reason when it failed, or an empty string.
[[nodiscard]] std::string jump_to_label(ScriptRuntime& runtime, std::string_view label);

}  // namespace vnmaki
