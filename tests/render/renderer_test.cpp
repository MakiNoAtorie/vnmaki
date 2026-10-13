// IRenderer through the SDL GPU backend, headless: each test renders into render targets, reads
// the pixels back and compares them with golden images from tools/generators/render_fixtures.py.
// The scenes below are the ones golden_scenes() in that script draws with its reference
// rasterizer.

#include "engine/render/renderer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/image.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "tests/render/gpu_test_support.hpp"
#include "tests/render/test_shaders.hpp"

namespace {

using vnmaki::BlendMode;
using vnmaki::kWholeTexture;
using vnmaki::rgba8;

constexpr vnmaki::Size kTargetSize{.width = 32, .height = 32};
const vnmaki::Color kGray = rgba8(26, 26, 26);
const vnmaki::Color kBlack = rgba8(0, 0, 0);
const vnmaki::Color kOpaqueWhite = rgba8(255, 255, 255);

constexpr vnmaki::RectF rect(float x, float y, float width, float height) {
  return vnmaki::RectF{.x = x, .y = y, .width = width, .height = height};
}

constexpr vnmaki::RectF kWholeTarget = rect(0, 0, 32, 32);

/// One quad of a golden scene.
struct SceneQuad {
  vnmaki::RectF destination{};
  vnmaki::RectF uv = kWholeTexture;
  vnmaki::Color tint = vnmaki::kWhite;
  BlendMode blend = BlendMode::kAlpha;
};

void draw_scene(vnmaki::IRenderer& renderer, vnmaki::TextureId texture,
                std::span<const SceneQuad> scene) {
  for (const SceneQuad& quad : scene) {
    renderer.draw(vnmaki::quad_from_rect(quad.destination, quad.uv, quad.tint),
                  {.texture = texture, .blend = quad.blend});
  }
}

/// A 32x32 nearest-filtered render target.
vnmaki::TextureId make_target(vnmaki::IRenderer& renderer) {
  return renderer.create_render_target(kTargetSize, vnmaki::TextureFilter::kNearest);
}

vnmaki::TextureId load_texture(vnmaki::IRenderer& renderer, const std::string& name) {
  return renderer.create_texture(vnmaki::test::load_fixture_image(name),
                                 vnmaki::TextureFilter::kNearest);
}

}  // namespace

TEST_CASE("renderer: a cleared render target reads back as the clear colour") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::Color clear = rgba8(51, 102, 153);
  const std::array<std::uint8_t, 4> clear_bytes{51, 102, 153, 255};

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = clear});
  const vnmaki::FrameStats stats = renderer->end_frame();
  const vnmaki::Image pixels = renderer->read_pixels(target);

  CHECK(stats.passes == 1);
  CHECK(stats.draw_calls == 0);
  REQUIRE(pixels.size == kTargetSize);
  std::vector<std::uint8_t> expected;
  expected.reserve(pixels.pixels.size());
  for (int i = 0; i < kTargetSize.width * kTargetSize.height; ++i) {
    expected.insert(expected.end(), clear_bytes.begin(), clear_bytes.end());
  }
  CHECK(pixels.pixels == expected);
}

TEST_CASE("renderer: textured quads show the texture scaled, cropped and tinted") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId checker = load_texture(*renderer, "checker_4x4.png");
  const std::array<SceneQuad, 3> scene{{
      {.destination = rect(4, 4, 16, 16)},
      // The top-right quarter of the texture.
      {.destination = rect(22, 4, 8, 8), .uv = rect(0.5F, 0, 0.5F, 0.5F)},
      {.destination = rect(4, 22, 8, 8), .tint = rgba8(255, 128, 64)},
  }};

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = kGray});
  draw_scene(*renderer, checker, scene);
  static_cast<void>(renderer->end_frame());

  vnmaki::test::check_golden(renderer->read_pixels(target), "textured_quad");
}

TEST_CASE("renderer: blend modes combine premultiplied colours") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId white = load_texture(*renderer, "white_1x1.png");
  const std::array<SceneQuad, 4> scene{{
      {.destination = rect(4, 4, 16, 16), .tint = rgba8(255, 0, 0, 128)},
      {.destination = rect(12, 12, 16, 16), .tint = rgba8(0, 0, 255, 64)},
      {.destination = rect(6, 6, 4, 4), .tint = rgba8(0, 64, 0), .blend = BlendMode::kAdditive},
      {.destination = rect(24, 2, 6, 6), .tint = rgba8(0, 0, 0, 0), .blend = BlendMode::kReplace},
  }};

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = kOpaqueWhite});
  draw_scene(*renderer, white, scene);
  static_cast<void>(renderer->end_frame());

  vnmaki::test::check_golden(renderer->read_pixels(target), "alpha_blend");
}

TEST_CASE("renderer: a render target can be drawn into and then drawn like a texture") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId checker = load_texture(*renderer, "checker_4x4.png");
  const vnmaki::Size layer_size{.width = 8, .height = 8};
  const vnmaki::TextureId layer =
      renderer->create_render_target(layer_size, vnmaki::TextureFilter::kNearest);
  CHECK(renderer->texture_size(layer) == layer_size);
  // The layer starts transparent; the checker covers only its middle.
  const std::array<SceneQuad, 1> layer_scene{{{.destination = rect(2, 2, 4, 4)}}};
  const std::array<SceneQuad, 1> scene{{{.destination = rect(8, 8, 16, 16)}}};

  renderer->begin_frame();
  renderer->begin_pass({.target = layer});
  draw_scene(*renderer, checker, layer_scene);
  renderer->begin_pass({.target = target, .clear = kGray});
  draw_scene(*renderer, layer, scene);
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.passes == 2);
  vnmaki::test::check_golden(renderer->read_pixels(target), "render_target");
}

TEST_CASE("renderer: a custom shader shades quads with per-draw uniforms") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId checker = load_texture(*renderer, "checker_4x4.png");
  const vnmaki::ShaderId grayscale =
      renderer->create_shader(vnmaki::test_shaders::grayscale_frag());
  struct GrayQuad {
    vnmaki::RectF destination{};
    float amount = 0;
  };
  const std::array<GrayQuad, 3> scene{{
      {.destination = rect(4, 4, 8, 8), .amount = 1.0F},
      {.destination = rect(20, 4, 8, 8), .amount = 0.0F},
      {.destination = rect(4, 20, 8, 8), .amount = 0.5F},
  }};

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = kGray});
  for (const GrayQuad& quad : scene) {
    const vnmaki::test_shaders::GrayscaleUniforms uniforms{.amount = quad.amount};
    renderer->draw(vnmaki::quad_from_rect(quad.destination),
                   {.texture = checker,
                    .shader = grayscale,
                    .uniforms = std::as_bytes(std::span{&uniforms, 1})});
  }
  const vnmaki::FrameStats stats = renderer->end_frame();

  // Different uniforms cannot share a draw call.
  CHECK(stats.draw_calls == 3);
  vnmaki::test::check_golden(renderer->read_pixels(target), "custom_shader");
}

TEST_CASE("renderer: sprites that share a texture are drawn with one draw call") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId checker = load_texture(*renderer, "checker_4x4.png");
  constexpr int kCells = 4;
  constexpr float kCellPixels = 8;
  constexpr float kSpritePixels = 6;
  constexpr float kCellUv = 0.25F;

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = kBlack});
  // Each texel of the checker as its own sprite, drawn one call at a time.
  for (int y = 0; y < kCells; ++y) {
    for (int x = 0; x < kCells; ++x) {
      const auto fx = static_cast<float>(x);
      const auto fy = static_cast<float>(y);
      renderer->draw(vnmaki::quad_from_rect(rect((fx * kCellPixels) + 1, (fy * kCellPixels) + 1,
                                                 kSpritePixels, kSpritePixels),
                                            rect(fx * kCellUv, fy * kCellUv, kCellUv, kCellUv)),
                     {.texture = checker});
    }
  }
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.quads == 16);
  CHECK(stats.draw_calls == 1);
  vnmaki::test::check_golden(renderer->read_pixels(target), "sprite_batch");
}

TEST_CASE("renderer: a change of texture, blend mode or pass starts a new draw call") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId other_target = make_target(*renderer);
  const vnmaki::TextureId checker = load_texture(*renderer, "checker_4x4.png");
  const vnmaki::TextureId white = load_texture(*renderer, "white_1x1.png");
  const vnmaki::Quad quad = vnmaki::quad_from_rect(rect(0, 0, 4, 4));

  SUBCASE("alternating textures") {
    renderer->begin_frame();
    renderer->begin_pass({.target = target});
    constexpr int kQuads = 10;
    for (int i = 0; i < kQuads; ++i) {
      renderer->draw(quad, {.texture = i % 2 == 0 ? checker : white});
    }
    CHECK(renderer->end_frame().draw_calls == kQuads);
  }
  SUBCASE("runs of one texture") {
    renderer->begin_frame();
    renderer->begin_pass({.target = target});
    renderer->draw(std::vector<vnmaki::Quad>(3, quad), {.texture = checker});
    renderer->draw(std::vector<vnmaki::Quad>(3, quad), {.texture = white});
    renderer->draw(std::vector<vnmaki::Quad>(3, quad), {.texture = checker});
    const vnmaki::FrameStats stats = renderer->end_frame();
    CHECK(stats.quads == 9);
    CHECK(stats.draw_calls == 3);
  }
  SUBCASE("blend modes") {
    renderer->begin_frame();
    renderer->begin_pass({.target = target});
    renderer->draw(quad, {.texture = white});
    renderer->draw(quad, {.texture = white, .blend = vnmaki::BlendMode::kAdditive});
    renderer->draw(quad, {.texture = white, .blend = vnmaki::BlendMode::kAdditive});
    CHECK(renderer->end_frame().draw_calls == 2);
  }
  SUBCASE("passes") {
    renderer->begin_frame();
    renderer->begin_pass({.target = target});
    renderer->draw(quad, {.texture = white});
    renderer->begin_pass({.target = other_target});
    renderer->draw(quad, {.texture = white});
    const vnmaki::FrameStats stats = renderer->end_frame();
    CHECK(stats.passes == 2);
    CHECK(stats.draw_calls == 2);
  }
  SUBCASE("the count is per frame") {
    for (int frame = 0; frame < 2; ++frame) {
      renderer->begin_frame();
      renderer->begin_pass({.target = target});
      renderer->draw(quad, {.texture = white});
      CHECK(renderer->end_frame().draw_calls == 1);
    }
  }
}

TEST_CASE("renderer: a batch longer than one draw call can index is split") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId white = load_texture(*renderer, "white_1x1.png");
  // 16-bit indices reach 65536 vertices: 16384 quads per draw call.
  constexpr std::size_t kQuads = 16384 + 1;

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = kBlack});
  const std::vector<vnmaki::Quad> quads(
      kQuads, vnmaki::quad_from_rect(rect(0, 0, 1, 1), vnmaki::kWholeTexture, rgba8(1, 1, 1)));
  renderer->draw(quads, {.texture = white, .blend = vnmaki::BlendMode::kAdditive});
  const vnmaki::FrameStats stats = renderer->end_frame();

  CHECK(stats.draw_calls == 2);
  // Every quad was drawn: 16385 additions of 1/255 saturate the top-left pixel.
  const vnmaki::Image pixels = renderer->read_pixels(target);
  CHECK(pixels.pixels.at(0) == 255);
  CHECK(pixels.pixels.at(4) == 0);
}

TEST_CASE("renderer: a texture destroyed during a frame stays usable until the frame ends") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId white = load_texture(*renderer, "white_1x1.png");

  renderer->begin_frame();
  renderer->begin_pass({.target = target, .clear = kBlack});
  renderer->draw(vnmaki::quad_from_rect(kWholeTarget), {.texture = white});
  renderer->destroy_texture(white);
  static_cast<void>(renderer->end_frame());

  CHECK(renderer->read_pixels(target).pixels.at(0) == 255);
  CHECK_THROWS_AS(static_cast<void>(renderer->texture_size(white)), vnmaki::RenderError);
}

TEST_CASE("renderer: misuse is reported as RenderError") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  const vnmaki::TextureId target = make_target(*renderer);
  const vnmaki::TextureId white = load_texture(*renderer, "white_1x1.png");
  const vnmaki::Quad quad = vnmaki::quad_from_rect(rect(0, 0, 4, 4));
  const vnmaki::ShaderId grayscale =
      renderer->create_shader(vnmaki::test_shaders::grayscale_frag());
  const std::array<std::byte, 16> some_uniforms{};

  CHECK_THROWS_AS(renderer->begin_pass({.target = target}), vnmaki::RenderError);
  renderer->begin_frame();
  CHECK_THROWS_AS(renderer->draw(quad, {.texture = white}), vnmaki::RenderError);
  CHECK_THROWS_AS(renderer->begin_pass({.target = vnmaki::kScreen}), vnmaki::RenderError);
  CHECK_THROWS_AS(renderer->begin_pass({.target = white}), vnmaki::RenderError);
  renderer->begin_pass({.target = target});
  CHECK_THROWS_AS(renderer->draw(quad, {.texture = vnmaki::TextureId{999}}), vnmaki::RenderError);
  CHECK_THROWS_AS(renderer->draw(quad, {.texture = target}), vnmaki::RenderError);
  CHECK_THROWS_AS(renderer->draw(quad, {.texture = white, .shader = vnmaki::ShaderId{999}}),
                  vnmaki::RenderError);
  CHECK_THROWS_AS(renderer->draw(quad, {.texture = white, .shader = grayscale}),
                  vnmaki::RenderError);
  CHECK_THROWS_AS(renderer->draw(quad, {.texture = white, .uniforms = some_uniforms}),
                  vnmaki::RenderError);
  CHECK_THROWS_AS(static_cast<void>(renderer->read_pixels(target)), vnmaki::RenderError);
  static_cast<void>(renderer->end_frame());
  CHECK_THROWS_AS(static_cast<void>(renderer->end_frame()), vnmaki::RenderError);
  CHECK_THROWS_AS(static_cast<void>(renderer->create_render_target(
                      vnmaki::Size{.width = 0, .height = 4}, vnmaki::TextureFilter::kLinear)),
                  vnmaki::RenderError);
  CHECK_THROWS_AS(static_cast<void>(renderer->create_texture(
                      vnmaki::Image{.size = {.width = 2, .height = 2}, .pixels = {1, 2, 3}},
                      vnmaki::TextureFilter::kLinear)),
                  vnmaki::RenderError);
}

TEST_CASE("renderer: a headless renderer has no screen and names its backend") {
  const auto renderer = vnmaki::test::make_headless_renderer();
  if (!renderer) {
    return;
  }
  CHECK(renderer->screen_size() == vnmaki::Size{});
  CHECK_FALSE(renderer->backend_name().empty());
  MESSAGE("GPU backend: " << renderer->backend_name());
}
