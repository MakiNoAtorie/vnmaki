#pragma once

// A renderer that does not draw: it records the quads, so a test sees what a displayable draws.

#include <span>
#include <string_view>
#include <vector>

#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"

namespace vnmaki::test {

// Draws as a renderer that only records the quads.
class RecordingRenderer final : public IRenderer {
 public:
  struct Draw {
    Quad quad;
    TextureId texture;
  };

  using IRenderer::draw;
  void draw(std::span<const Quad> quads, const DrawState& state) override {
    for (const Quad& quad : quads) {
      draws.push_back({.quad = quad, .texture = state.texture});
    }
  }
  [[nodiscard]] std::string_view backend_name() const override { return "recording"; }
  [[nodiscard]] TextureId create_texture(const Image&, TextureFilter) override { return {}; }
  [[nodiscard]] TextureId create_render_target(Size, TextureFilter) override { return {}; }
  [[nodiscard]] ShaderId create_shader(const ShaderBytecode&) override { return {}; }
  void destroy_texture(TextureId) override {}
  void destroy_shader(ShaderId) override {}
  [[nodiscard]] Size texture_size(TextureId) const override { return {}; }
  [[nodiscard]] Size screen_size() const override { return {}; }
  void begin_frame() override {}
  void begin_pass(const RenderPass&) override {}
  FrameStats end_frame() override { return {}; }
  [[nodiscard]] Image read_pixels(TextureId) override { return {}; }

  std::vector<Draw> draws;
};

}  // namespace vnmaki::test
