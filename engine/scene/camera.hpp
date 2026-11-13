#pragma once

// The camera (docs/SPEC.md section 10): a pan, zoom and rotation of the layers from the background
// to the overlay, a parallax factor for the pan of each layer, and a shake. The camera is plain
// data; Scene keeps it, tweens it as the tag "@camera" and draws through a CameraRenderer.

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

/// `tween` and `wait` address the camera with this tag.
inline constexpr std::string_view kCameraTag = "@camera";

/// The layers the camera moves: background, master, characters, 3d and overlay. The ui and
/// transient layers stay where they are.
inline constexpr std::size_t kCameraLayerCount = 5;

/// The smallest zoom that is drawn; a back easing may overshoot to or below 0.
inline constexpr float kMinZoom = 0.01F;

/// The largest parallax factor, either way.
inline constexpr double kMaxParallax = 100.0;

/// Where the camera looks. `pose` is a DisplayProperties so the timeline tweens it: `position` is
/// the pan in virtual pixels (the view moves right and down, so the content moves left and up),
/// `scale.x` is the zoom (1 is none, 2 shows the content twice as large) and `rotation` turns the
/// content clockwise, in degrees. Zoom and rotation are about the centre of the screen. Nothing
/// else of `pose` is used.
struct CameraState {
  DisplayProperties pose{};
  /// The pan of each layer is `parallax` times the camera's pan (and its shake).
  std::array<float, kCameraLayerCount> parallax{1, 1, 1, 1, 1};

  /// True if the camera shows the layers as they are.
  [[nodiscard]] bool is_default() const;
  bool operator==(const CameraState&) const = default;
};

/// A shake that is running. Its offset is a fixed function of `elapsed` and the other fields.
struct Shake {
  float strength = 0;   ///< The largest offset, in virtual pixels, at the start.
  double duration = 0;  ///< Seconds until it has died away.
  double elapsed = 0;   ///< Seconds since it started.

  [[nodiscard]] bool done() const { return elapsed >= duration; }
};

/// The shake offset in virtual pixels at `shake.elapsed`: smooth noise of the time (a fixed
/// function, never `rand`) times a factor that falls linearly from 1 to 0 over the duration.
[[nodiscard]] PointF shake_offset(const Shake& shake);

/// How the camera maps a point of a layer to the screen.
class CameraView {
 public:
  /// The view of layer number `layer` (below kCameraLayerCount) with the pan of `camera` and the
  /// extra pan `shake`, in a virtual screen of `virtual_size`.
  CameraView(const CameraState& camera, PointF shake, Size virtual_size, std::size_t layer);

  [[nodiscard]] PointF apply(PointF point) const;
  /// True if apply() returns every point unchanged.
  [[nodiscard]] bool identity() const { return identity_; }

 private:
  PointF center_{};
  PointF pan_{};
  float zoom_ = 1;
  float cosine_ = 1;
  float sine_ = 0;
  bool identity_ = true;
};

/// An IRenderer that moves the quads of every draw by a CameraView and passes everything else on.
/// `inner` must outlive it.
class CameraRenderer final : public IRenderer {
 public:
  using IRenderer::draw;

  CameraRenderer(IRenderer& inner, const CameraView& view) : inner_(inner), view_(view) {}

  [[nodiscard]] std::string_view backend_name() const override { return inner_.backend_name(); }
  [[nodiscard]] TextureId create_texture(const Image& image, TextureFilter filter) override {
    return inner_.create_texture(image, filter);
  }
  [[nodiscard]] TextureId create_render_target(Size size, TextureFilter filter) override {
    return inner_.create_render_target(size, filter);
  }
  [[nodiscard]] ShaderId create_shader(const ShaderBytecode& fragment) override {
    return inner_.create_shader(fragment);
  }
  void destroy_texture(TextureId texture) override { inner_.destroy_texture(texture); }
  void destroy_shader(ShaderId shader) override { inner_.destroy_shader(shader); }
  [[nodiscard]] Size texture_size(TextureId texture) const override {
    return inner_.texture_size(texture);
  }
  [[nodiscard]] Size screen_size() const override { return inner_.screen_size(); }
  void begin_frame() override { inner_.begin_frame(); }
  void begin_pass(const RenderPass& pass) override { inner_.begin_pass(pass); }
  void draw(std::span<const Quad> quads, const DrawState& state) override;
  FrameStats end_frame() override { return inner_.end_frame(); }
  [[nodiscard]] Image read_pixels(TextureId texture) override {
    return inner_.read_pixels(texture);
  }

 private:
  IRenderer& inner_;
  CameraView view_;
  std::vector<Quad> moved_;
};

}  // namespace vnmaki
