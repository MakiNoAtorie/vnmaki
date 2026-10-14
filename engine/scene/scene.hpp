#pragma once

// The scene (docs/SPEC.md sections 7 and 9): a stack of layers holding tagged displayables. The
// script calls `scene`, `show` and `hide` change it, snapshots capture it, and the engine draws
// it each frame.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

class Vfs;

/// The layers, in the order they are declared and drawn: kBackground first (at the back).
enum class Layer : std::uint8_t {
  kBackground,
  kMaster,
  kCharacters,
  k3d,
  kOverlay,
  kUi,
  kTransient,
};

/// Every layer, back to front.
inline constexpr std::array kLayerOrder{
    Layer::kBackground, Layer::kMaster, Layer::kCharacters, Layer::k3d,
    Layer::kOverlay,    Layer::kUi,     Layer::kTransient,
};

/// The name scripts use: "background", "master", "characters", "3d", "overlay", "ui", "transient".
[[nodiscard]] std::string_view layer_name(Layer layer);
[[nodiscard]] std::optional<Layer> layer_from_name(std::string_view name);

/// The largest `x` or `y` a script may give, as a fraction of the virtual resolution.
inline constexpr double kMaxPositionFraction = 1'000'000.0;

/// The tag of the displayable `scene(image)` puts on the background layer.
inline constexpr std::string_view kBackgroundTag = "background";

/// What is shown: which displayable has which tag, on which layer, with which properties.
///
/// A tag names at most one displayable in the whole scene. Within a layer, displayables draw in
/// the order they were first shown. Positions are virtual pixels.
///
/// `show(tag, options)` shows image `options.image` (the tag itself without it) on
/// `options.layer` (the tag's current layer, or "characters"). `at` is "left", "center" or
/// "right": a quarter, a half or three quarters across, standing on the bottom edge (the anchor
/// is the image's bottom centre). `x` and `y` then override either coordinate as a fraction of
/// the virtual resolution (at most kMaxPositionFraction either way). A new tag starts at "center";
/// a tag already shown keeps the properties the call does not set, and its place in the drawing
/// order if the layer is the same. `scene(image)` empties the background, master and characters
/// layers and puts the image on the background with its top-left corner at the origin. `hide` of a
/// tag not shown does nothing.
class Scene final : public SceneState, public SceneCommands {
 public:
  /// `vfs` must outlive the scene. `virtual_size` is the virtual resolution.
  Scene(const Vfs& vfs, Size virtual_size);

  [[nodiscard]] std::string scene(std::string_view image) override;
  [[nodiscard]] std::string show(const ShowRequest& request) override;
  void hide(std::string_view tag) override;

  [[nodiscard]] std::vector<std::byte> capture() const override;
  void restore(std::span<const std::byte> state) override;

  /// The tags on `layer`, back to front.
  [[nodiscard]] std::vector<std::string> tags(Layer layer) const;
  [[nodiscard]] std::optional<Layer> layer_of(std::string_view tag) const;
  /// The displayable with this tag, or null. Invalidated by the next call that changes what is
  /// shown.
  [[nodiscard]] ImageDisplayable* find(std::string_view tag);
  [[nodiscard]] const ImageDisplayable* find(std::string_view tag) const;

  /// Advances every displayable by `seconds`.
  void update(double seconds);
  /// Loads and uploads the images of displayables that have no texture yet. Call it on the main
  /// thread before draw(), always with the same renderer (the scene keeps its textures). Rethrows
  /// ImageError for a file that cannot be decoded and RenderError.
  void prepare(IRenderer& renderer, JobSystem& jobs);
  /// Draws every layer, back to front, into the renderer's current pass.
  void draw(IRenderer& renderer) const;

 private:
  struct Entry {
    std::string tag;
    ImageDisplayable displayable;
  };
  struct Texture {
    TextureId id{};
    Size size{};
  };
  struct Slot {
    Layer layer = Layer::kBackground;
    std::size_t index = 0;
  };
  using Layers = std::array<std::vector<Entry>, kLayerOrder.size()>;

  [[nodiscard]] std::optional<Slot> locate(std::string_view tag) const;

  [[nodiscard]] std::vector<Entry>& entries(Layer layer);
  [[nodiscard]] const std::vector<Entry>& entries(Layer layer) const;

  const Vfs* vfs_;
  Size virtual_size_;
  Layers layers_;
  std::map<std::string, Texture, std::less<>> textures_;
};

}  // namespace vnmaki
