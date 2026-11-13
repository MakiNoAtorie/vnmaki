#pragma once

// Layered sprites (docs/SPEC.md sections 7 and 10): one character made of image layers. Each
// group of layers (base, outfit, face, ...) shows one of its images, and the script changes the
// groups it names with `show`.

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

/// What `layered(name, ...)` defines. The images are Vfs paths.
struct LayeredDefinition {
  struct Value {
    std::string name{};
    std::string path{};
  };
  /// One group of images, of which one shows. A group of one fixed image has one value with an
  /// empty name, and it is always selected.
  struct Group {
    std::string name{};
    std::vector<Value> values{};
  };

  std::string name{};
  std::vector<Group> groups{};  ///< From the back to the front.
  /// (group, value) chosen when the sprite is made. A group without one shows nothing.
  std::vector<std::pair<std::string, std::string>> defaults{};
  /// Seconds that a changed group takes to fade from its old image to its new image.
  double crossfade = 0;
};

/// (group, value) pairs.
using LayerAttributes = std::vector<std::pair<std::string, std::string>>;

/// The layers draw in the order of the groups, all with their top-left corner at the sprite's
/// origin. The first group is the base: its image is the size that `anchor`, `scale`, `crop` and
/// bounds() measure. A layer with another size keeps its own size.
///
/// A changed group fades in its new image over its old image while update() moves the clock on
/// (the one that moves tweens).
class LayeredSprite final : public Displayable {
 public:
  /// Selects the defaults of `definition`.
  explicit LayeredSprite(std::shared_ptr<const LayeredDefinition> definition);

  [[nodiscard]] const std::string& name() const { return definition_->name; }
  /// The (group, value) of each group that shows an image, from the back to the front.
  [[nodiscard]] LayerAttributes attributes() const;
  /// Changes the groups named in `changes`. Returns why it cannot (an unknown group or value),
  /// and then changes nothing. With `crossfade` a changed group fades if the definition has a
  /// crossfade time; without it, the change is a cut and ends the fades that are running.
  [[nodiscard]] std::string set_attributes(const LayerAttributes& changes, bool crossfade);
  /// How far the fade of `group` is, from 0 to 1, or nothing when it does not fade.
  [[nodiscard]] std::optional<float> fade_progress(std::string_view group) const;

  /// The paths of the images that draw now and have no texture yet.
  [[nodiscard]] std::vector<std::string> missing_paths() const;
  /// `size` is the image's size in pixels; one image pixel is one virtual pixel at scale 1.
  void set_texture(const std::string& path, TextureId texture, Size size);

  void update(double seconds) override;
  void draw(IRenderer& renderer) const override;
  /// The base image with the properties applied. Empty until the base has its texture.
  [[nodiscard]] RectF bounds() const override;

 private:
  struct Layer {
    std::optional<std::size_t> current{};   ///< Index in the group's values.
    std::optional<std::size_t> previous{};  ///< What the fade started from.
    bool fading = false;
    double elapsed = 0;
  };
  struct Texture {
    TextureId id{};
    Size size{};
  };

  [[nodiscard]] const std::string& path_of(std::size_t group, std::size_t value) const;
  [[nodiscard]] const Texture* texture_of(const std::string& path) const;
  [[nodiscard]] Size base_size() const;
  void draw_layer(IRenderer& renderer, const std::string& path, Size base, float opacity) const;

  std::shared_ptr<const LayeredDefinition> definition_;
  std::vector<Layer> layers_;
  std::map<std::string, Texture, std::less<>> textures_;
};

}  // namespace vnmaki
