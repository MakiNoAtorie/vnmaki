#pragma once

// The displayable interface (docs/SPEC.md section 6): everything visible implements it, so the
// scene and the animation system treat images, text, video and the rest the same way.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki {

/// The most uniforms a game shader may declare.
inline constexpr std::size_t kMaxUniforms = 16;

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
  /// The game shader that draws the displayable (the number Scene gives it, from 1), or 0 for the
  /// default one, and the values of its uniforms in the order `shader(name, {...})` declared them.
  std::uint32_t shader = 0;
  std::array<float, kMaxUniforms> uniforms{};

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

  /// Sets the GPU shader that draw() uses. `uniform_floats` of the uniform values go to its
  /// constant buffer (0 for a shader without one). Scene::prepare() calls it.
  void set_shader(ShaderId shader, std::size_t uniform_floats) {
    shader_ = shader;
    uniform_floats_ = uniform_floats;
  }

 protected:
  /// What a draw of `texture` needs: the shader and its uniform values as the properties have
  /// them now.
  [[nodiscard]] DrawState draw_state(TextureId texture) const;

 private:
  DisplayProperties properties_{};
  ShaderId shader_{};
  std::size_t uniform_floats_ = 0;
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
