// Text through the whole pipeline on a real GPU device: layout, glyph atlas, quads, pixels. The
// golden image is drawn by draw_text() in tools/generators/render_fixtures.py from the box font's
// glyph table, without any of the text libraries.

#include <cstddef>
#include <filesystem>
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
