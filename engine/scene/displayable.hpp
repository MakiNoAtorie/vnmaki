#pragma once

// The displayable interface (docs/SPEC.md section 6): everything visible implements it, so the
// scene and the animation system treat images, text, video and the rest the same way.

#include <string>
#include <utility>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki {

/// The properties every displayable has. The animation system changes them over time; they are
/// plain values so a tween can read, interpolate and write each one.
struct DisplayProperties {
  /// Where the anchor point lands, in virtual pixels (origin top-left, y down).
  PointF position{};
  /// The point of the displayable that `position` places and that it scales and rotates around,
  /// as a fraction of its own size: (0, 0) is its top-left corner, (1, 1) its bottom-right.
  PointF anchor{};
  PointF scale{.x = 1, .y = 1};
  /// Degrees, clockwise on screen.
  float rotation = 0;
  /// Opacity in [0, 1]; multiplies the tint's alpha.
  float alpha = 1;
  /// Multiplies the displayable's colour (straight alpha; white leaves it unchanged).
  Color tint = kWhite;
  /// The part of the displayable to show, as fractions of its full size. The shown part is what
  /// `anchor`, `scale` and bounds() measure.
  RectF crop = kWholeTexture;

  bool operator==(const DisplayProperties&) const = default;
};

/// Something visible. The scene owns displayables, updates them once per fixed step and draws
/// them back to front.
class Displayable {
 public:
  Displayable() = default;
  Displayable(const Displayable&) = default;
  Displayable(Displayable&&) = default;
  Displayable& operator=(const Displayable&) = default;
  Displayable& operator=(Displayable&&) = default;
  virtual ~Displayable() = default;

  [[nodiscard]] DisplayProperties& properties() { return properties_; }
  [[nodiscard]] const DisplayProperties& properties() const { return properties_; }

  /// Advances the displayable's own animation (video frames, puppets) by `seconds`.
  virtual void update(double seconds) = 0;
  /// Draws into the renderer's current pass, whose target has the virtual resolution.
  virtual void draw(IRenderer& renderer) const = 0;
  /// The axis-aligned box, in virtual pixels, that the displayable covers with its properties
  /// applied.
  [[nodiscard]] virtual RectF bounds() const = 0;

 private:
  DisplayProperties properties_{};
};

/// One image file. It draws nothing and has an empty size until set_texture() gives it the
/// uploaded image; Scene::prepare() does that.
class ImageDisplayable final : public Displayable {
 public:
  /// `path` is the image's virtual path in the Vfs.
  explicit ImageDisplayable(std::string path) : path_(std::move(path)) {}

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] bool has_texture() const { return texture_ != TextureId{}; }
  /// `size` is the image's size in pixels; one image pixel is one virtual pixel at scale 1.
  void set_texture(TextureId texture, Size size);

  void update(double /*seconds*/) override {}
  void draw(IRenderer& renderer) const override;
  [[nodiscard]] RectF bounds() const override;

 private:
  std::string path_;
  TextureId texture_{};
  Size size_{};
};

}  // namespace vnmaki
