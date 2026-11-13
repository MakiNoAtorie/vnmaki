// The say box on a real GPU device: box, speaker name in her colour and a partly revealed line. The
// golden image is drawn by draw_say_box() in tools/generators/render_fixtures.py from the box
// font's glyph table, without any of the text libraries.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>

#include <doctest/doctest.h>

#include "engine/dialogue/dialogue_presenter.hpp"
#include "engine/dialogue/say_box.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/text/text.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kTargetSize{.width = 320, .height = 180};
constexpr std::uint32_t kLilac = 0xC8A2C8;
constexpr std::size_t kRevealed = 9;  // "You came " of "You came back."
constexpr float kNameSize = 20;
constexpr float kTextSize = 40;
constexpr float kBoxLeft = 10;
constexpr float kBoxTop = 70;
constexpr float kBoxWidth = 300;
constexpr float kBoxHeight = 100;
constexpr float kNameLeft = 8;
constexpr float kNameTop = 6;
constexpr float kTextTop = 30;
constexpr float kWrapWidth = 284;
constexpr std::uint8_t kBoxRed = 16;
constexpr std::uint8_t kBoxGreen = 16;
constexpr std::uint8_t kBoxBlue = 32;
constexpr std::uint8_t kBoxAlpha = 204;
constexpr std::uint8_t kBackdrop = 26;

vnmaki::SayBoxStyle test_style() {
  return {.box = {.x = kBoxLeft, .y = kBoxTop, .width = kBoxWidth, .height = kBoxHeight},
          .box_color = vnmaki::rgba8(kBoxRed, kBoxGreen, kBoxBlue, kBoxAlpha),
          .name_offset = {.x = kNameLeft, .y = kNameTop},
          .text_offset = {.x = kNameLeft, .y = kTextTop},
          .name_size = kNameSize,
          .text_size = kTextSize,
          .wrap_width = kWrapWidth,
          .text_color = vnmaki::kWhite};
}

}  // namespace

TEST_CASE("say box: the panel, the coloured name and a partly revealed line") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");
  const vnmaki::DialogueLine line{
      .speaker = "Eileen", .color = std::optional<std::uint32_t>{kLilac}, .text = "You came back."};

  const vnmaki::TextureId target =
      renderer->create_render_target(kTargetSize, vnmaki::TextureFilter::kNearest);
  vnmaki::SayBoxView view{*renderer, font, test_style()};
  REQUIRE(view.glyph_count(line) == line.text.size());

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(kBackdrop, kBackdrop, kBackdrop)});
  view.draw(line, kRevealed, 1);
  const vnmaki::FrameStats stats = renderer->end_frame();

  // The panel, the name and the line each use their own texture.
  CHECK(stats.draw_calls == 3);
  vnmaki::test::check_golden(renderer->read_pixels(target), "say_box");
}

TEST_CASE("say box: narration has no name and the same panel") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  vnmaki::Vfs vfs;
  REQUIRE(
      vfs.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} / "text")));
  const vnmaki::Font font = vnmaki::load_font(vfs, "fonts/boxes.ttf");
  const vnmaki::DialogueLine line{.speaker = {}, .color = std::nullopt, .text = "Rain."};

  const vnmaki::TextureId target =
      renderer->create_render_target(kTargetSize, vnmaki::TextureFilter::kNearest);
  vnmaki::SayBoxView view{*renderer, font, test_style()};
  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = vnmaki::rgba8(kBackdrop, kBackdrop, kBackdrop)});
  view.draw(line, view.glyph_count(line), 1);
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.draw_calls == 2);  // the panel and the line
}
