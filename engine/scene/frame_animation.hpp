#pragma once

// Frame animation (docs/SPEC.md section 10): an image sequence or a sprite sheet that plays at a
// fixed rate. It is a displayable of its own, and a value of a layered sprite group can be one.

#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

/// What `animation(name, ...)` defines. The images are Vfs paths.
struct AnimationDefinition {
  /// One picture: the file and the part of it (the whole file, or a cell of a sheet).
  struct Frame {
    std::string path{};
    RectF uv = kWholeTexture;
  };

  std::string name{};
  std::vector<Frame> frames{};  ///< At least one.
  /// The cells of the sheet that the frames come from; 1 and 1 for an image sequence.
  int columns = 1;
  int rows = 1;
  double fps = 1;
  bool loop = true;

  /// The index of the frame shown `elapsed` seconds after the start: it wraps when `loop`, and
  /// stays at the last frame otherwise.
  [[nodiscard]] std::size_t frame_at(double elapsed) const;
  /// Seconds that one pass takes.
  [[nodiscard]] double duration() const { return static_cast<double>(frames.size()) / fps; }
  /// True when the animation does not loop and its pass is not over `elapsed` seconds after the
  /// start.
  [[nodiscard]] bool running_at(double elapsed) const { return !loop && elapsed < duration(); }
  /// The size of one frame whose file is `texture` big.
  [[nodiscard]] Size frame_size(Size texture) const {
    return Size{.width = texture.width / columns, .height = texture.height / rows};
  }
};

/// `part`, given as fractions of a frame, as fractions of the whole file: `frame` is the frame's
/// own uv rectangle.
[[nodiscard]] RectF uv_within(const RectF& frame, const RectF& part);

/// A displayable that plays an animation from frame 1 on the clock of update(). A loop never
/// ends. Otherwise the last frame stays when the pass is over. Its images load as for an
/// ImageDisplayable: Scene::prepare() gives them with set_texture().
class FrameAnimation final : public Displayable {
 public:
  explicit FrameAnimation(std::shared_ptr<const AnimationDefinition> definition)
      : definition_(std::move(definition)) {}

  [[nodiscard]] const std::string& name() const { return definition_->name; }
  /// The index of the frame that shows.
  [[nodiscard]] std::size_t frame() const { return definition_->frame_at(elapsed_); }
  /// True while a pass that ends is still running: what `wait` waits for.
  [[nodiscard]] bool playing() const { return definition_->running_at(elapsed_); }
  /// Puts a pass that ends at its end.
  void finish();

  /// The paths of the images that the frames need and that have no texture yet.
  [[nodiscard]] std::vector<std::string> missing_paths() const;
  /// `size` is the image's size in pixels; one image pixel is one virtual pixel at scale 1.
  void set_texture(const std::string& path, TextureId texture, Size size);

  void update(double seconds) override { elapsed_ += seconds; }
  void draw(IRenderer& renderer) const override;
  /// The current frame with the properties applied. Empty until it has its texture.
  [[nodiscard]] RectF bounds() const override;

 private:
  struct Texture {
    TextureId id{};
    Size size{};
  };

  [[nodiscard]] const Texture* current_texture() const;

  std::shared_ptr<const AnimationDefinition> definition_;
  std::map<std::string, Texture, std::less<>> textures_;
  double elapsed_ = 0;
};

}  // namespace vnmaki
