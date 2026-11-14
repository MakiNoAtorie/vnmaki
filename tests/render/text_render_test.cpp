// Text through the whole pipeline on a real GPU device: layout, glyph atlas, quads, pixels. The
// golden image is drawn by draw_text() in tools/generators/render_fixtures.py from the box font's
// glyph table, without any of the text libraries.

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/text.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

TEST_CASE("text: a wrapped, partly revealed paragraph draws sharp glyphs") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");

  constexpr std::string_view kText = "Hello, VN. こんにちは、世界。";
  constexpr float kPixelSize = 20;
  constexpr float kWrapWidth = 100;
  const vnmaki::TextLayout layout = vnmaki::layout_text(font, kText, kPixelSize, kWrapWidth);
  REQUIRE(layout.glyph_count == 20);
  vnmaki::GlyphAtlas atlas{font};
  REQUIRE(atlas.prepare(layout));

  // A second and a half into a reveal at 12 characters per second: 18 of the 20 glyphs.
  constexpr double kCharactersPerSecond = 12;
  constexpr double kElapsedSeconds = 1.5;
  const std::size_t visible =
      vnmaki::revealed_glyphs(layout.glyph_count, kCharactersPerSecond, kElapsedSeconds);
  constexpr vnmaki::PointF kOrigin{.x = 6, .y = 2};
  const vnmaki::Color tint = vnmaki::rgba8(255, 224, 128);
  const std::vector<vnmaki::Quad> quads = vnmaki::text_quads(layout, atlas, kOrigin, visible, tint);

  constexpr vnmaki::Size kTargetSize{.width = 112, .height = 64};
  const vnmaki::Color gray = vnmaki::rgba8(26, 26, 26);
  const vnmaki::TextureId target =
      renderer->create_render_target(kTargetSize, vnmaki::TextureFilter::kNearest);
  const vnmaki::TextureId glyphs =
      renderer->create_texture(atlas.image(), vnmaki::TextureFilter::kNearest);

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = gray});
  renderer->draw(quads, {.texture = glyphs});
  const vnmaki::FrameStats stats = renderer->end_frame();

  // The whole paragraph is one draw call: every glyph comes from the one atlas texture.
  CHECK(stats.draw_calls == 1);
  vnmaki::test::check_golden(renderer->read_pixels(target), "text_paragraph");
}

// The golden image is drawn by draw_sdf_text() in tools/generators/render_fixtures.py, which
// repeats FreeType's distance values for a box and the shader's arithmetic.
TEST_CASE("text: effect text has an outline and a shadow from a distance-field atlas") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");

  constexpr float kPixelSize = 20;
  const vnmaki::TextLayout layout = vnmaki::layout_text(font, "Ab", kPixelSize, vnmaki::kNoWrap);
  vnmaki::GlyphAtlas atlas{vnmaki::GlyphRaster::kDistance};
  REQUIRE(atlas.prepare(layout));

  constexpr float kOutlineWidth = 2;
  constexpr vnmaki::PointF kOrigin{.x = 12, .y = 8};
  constexpr vnmaki::PointF kShadowOrigin{.x = 15, .y = 11};
  const vnmaki::Color tint = vnmaki::rgba8(255, 224, 128);
  const vnmaki::Color shadow = vnmaki::rgba8(0, 0, 0, 128);
  const vnmaki::SdfTextUniforms text_uniforms =
      vnmaki::sdf_text_uniforms(vnmaki::rgba8(32, 48, 160), kOutlineWidth);
  // The shadow's outline has the shadow's colour, so the shadow is as wide as the outlined text.
  const vnmaki::SdfTextUniforms shadow_uniforms =
      vnmaki::sdf_text_uniforms(vnmaki::rgba8(0, 0, 0), kOutlineWidth);

  constexpr vnmaki::Size kTargetSize{.width = 48, .height = 40};
  const vnmaki::Color gray = vnmaki::rgba8(26, 26, 26);
  const vnmaki::TextureId target =
      renderer->create_render_target(kTargetSize, vnmaki::TextureFilter::kNearest);
  const vnmaki::TextureId glyphs =
      renderer->create_texture(atlas.image(), vnmaki::TextureFilter::kNearest);
  const vnmaki::ShaderId shader = renderer->create_shader(vnmaki::sdf_text_shader());

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = gray});
  renderer->draw(vnmaki::text_quads(layout, atlas, kShadowOrigin, layout.glyph_count, shadow),
                 {.texture = glyphs,
                  .shader = shader,
                  .uniforms = std::as_bytes(std::span{&shadow_uniforms, 1})});
  renderer->draw(vnmaki::text_quads(layout, atlas, kOrigin, layout.glyph_count, tint),
                 {.texture = glyphs,
                  .shader = shader,
                  .uniforms = std::as_bytes(std::span{&text_uniforms, 1})});
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.draw_calls == 2);
  // One step of the 8-bit distance is 1/16 pixel, which the soft edge turns into a few levels
  // of colour.
  constexpr int kTolerance = 4;
  vnmaki::test::check_golden(renderer->read_pixels(target), "text_effect", kTolerance);
}
