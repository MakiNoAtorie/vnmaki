// 3D scenes on a real GPU device: each label of the generated project model3d/game
// (tools/generators/model_fixtures.py) builds a scene of cubes, drawn at 64x64 with a camera that
// looks straight at the cubes. The unlit frame must match the golden of the generator, which needs
// no triangle rasterizer for it; the lit frames are checked at a pixel, with a tolerance, against
// the shading rules worked out here.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

#include <doctest/doctest.h>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/render/gpu_test_support.hpp"

namespace {

constexpr vnmaki::Size kVirtualSize{.width = 64, .height = 64};
constexpr int kCenter = 32;  // the pixel just right of and below the middle of the screen
constexpr int kTolerance = 3;
constexpr int kFarCubeX = 20;    // in the far cube's face, left of the near cube's
constexpr int kCornerPixel = 4;  // outside both cubes
// The front face covers pixels 24 to 40: a pixel in each quarter, away from the middle.
constexpr int kLeftQuarter = 26;
constexpr int kRightQuarter = 38;
constexpr int kFull = 255;
constexpr std::size_t kBytesPerPixel = 4;
constexpr float kByteMax = 255.0F;

// The orange of cube.gltf (0.8, 0.4, 0.2) and the blue of blue.gltf (0.2, 0.4, 0.8) in bytes.
constexpr std::array<int, 3> kOrange{204, 102, 51};
constexpr std::array<int, 3> kBlue{51, 102, 204};

// The light of the toon and PBR labels: an ambient light of 0.1 and a directional light that
// faces the cube's front.
constexpr float kAmbient = 0.1F;
constexpr float kSunToon = 0.8F;
constexpr float kSunPbr = 0.5F;
// The bands of shaders/mesh_toon.frag.hlsl.
constexpr float kBrightBand = 1.0F;
constexpr float kMiddleBand = 0.65F;
constexpr float kDarkBand = 0.35F;
// The point light of toon_point: intensity 8.5 at distance 4 weakens to half.
constexpr float kBulbRadiance = 0.5F;
// pbr_metal: the material is fully metallic and 0.2 rough, so the highlight is 1 - roughness.
constexpr float kMetalShine = 0.8F;
constexpr float kWhitenAmount = 0.5F;

std::array<int, 3> scaled(const std::array<int, 3>& color, float factor) {
  std::array<int, 3> result{};
  for (std::size_t i = 0; i < color.size(); ++i) {
    result.at(i) = static_cast<int>(std::lround(
        std::min(1.0F, static_cast<float>(color.at(i)) * factor / kByteMax) * kByteMax));
  }
  return result;
}

// Draws the story's `label` and returns the 64x64 frame, or no image without a GPU.
class Frame {
 public:
  explicit Frame(const std::string& label)
      : renderer_(vnmaki::test::make_headless_renderer()), scene_(vfs_, kVirtualSize) {
    if (!renderer_) {
      return;
    }
    REQUIRE(vfs_.mount(vnmaki::make_directory_mount(std::filesystem::path{VNMAKI_FIXTURES_DIR} /
                                                    "model3d" / "game")));
    vnmaki::ScriptRuntimeOptions options;
    options.scene = &scene_;
    options.scene_state = &scene_;
    vnmaki::ScriptRuntime runtime{vfs_, options};
    runtime.load_scripts();
    runtime.start(label);
    REQUIRE_MESSAGE(runtime.state() == vnmaki::RunState::kWaiting, runtime.error());
    const vnmaki::TextureId target =
        renderer_->create_render_target(kVirtualSize, vnmaki::TextureFilter::kNearest);
    scene_.prepare(*renderer_, jobs_);
    renderer_->begin_frame();
    renderer_->begin_pass({.target = target, .clear = vnmaki::rgba8(0, 0, 0)});
    scene_.draw(*renderer_, target);
    static_cast<void>(renderer_->end_frame());
    image_ = renderer_->read_pixels(target);
    ok_ = true;
  }

  [[nodiscard]] bool ok() const { return ok_; }
  [[nodiscard]] const vnmaki::Image& image() const { return image_; }

  // The red, green and blue bytes of a pixel.
  [[nodiscard]] std::array<int, 3> pixel(int x, int y) const {
    const auto row = static_cast<std::size_t>(y);
    const auto column = static_cast<std::size_t>(x);
    const auto at =
        ((row * static_cast<std::size_t>(kVirtualSize.width)) + column) * kBytesPerPixel;
    return {image_.pixels.at(at), image_.pixels.at(at + 1), image_.pixels.at(at + 2)};
  }

  void expect(int x, int y, const std::array<int, 3>& color) const {
    const std::array<int, 3> actual = pixel(x, y);
    INFO("pixel (", x, ", ", y, ")");
    for (std::size_t i = 0; i < color.size(); ++i) {
      CHECK(std::abs(actual.at(i) - color.at(i)) <= kTolerance);
    }
  }

 private:
  std::unique_ptr<vnmaki::SdlGpuRenderer> renderer_;
  vnmaki::Vfs vfs_;
  vnmaki::Scene scene_;
  vnmaki::JobSystem jobs_;
  vnmaki::Image image_;
  bool ok_ = false;
};

}  // namespace

TEST_CASE("model3d: unlit cubes match the golden, and the depth buffer hides the far cube") {
  const Frame frame{"unlit"};
  if (!frame.ok()) {
    return;
  }
  vnmaki::test::check_golden_in("model3d", frame.image(), "unlit");
  frame.expect(kCenter, kCenter, kOrange);              // the near cube, drawn first, in front
  frame.expect(kFarCubeX, kCenter, kBlue);              // the far cube, drawn second, around it
  frame.expect(kCornerPixel, kCornerPixel, {0, 0, 0});  // nothing: the 2D background shows
}

TEST_CASE("model3d: toon shading falls into three bands") {
  const std::array<std::pair<const char*, float>, 3> cases{
      {{"toon_bright", kBrightBand}, {"toon_middle", kMiddleBand}, {"toon_dark", kDarkBand}}};
  for (const auto& [label, band] : cases) {
    CAPTURE(label);
    const Frame frame{label};
    if (!frame.ok()) {
      return;
    }
    frame.expect(kCenter, kCenter, scaled(kOrange, kAmbient + (kSunToon * band)));
  }
}

TEST_CASE("model3d: a point light weakens with the square of its distance") {
  const Frame frame{"toon_point"};
  if (!frame.ok()) {
    return;
  }
  frame.expect(kCenter, kCenter, scaled(kOrange, kAmbient + (kBulbRadiance * kBrightBand)));
}

TEST_CASE("model3d: PBR shades a rough dielectric and a smooth metal differently") {
  const Frame rough{"pbr_rough"};
  if (!rough.ok()) {
    return;
  }
  rough.expect(kCenter, kCenter, scaled(kOrange, kAmbient + kSunPbr));
  // The metal has no diffuse light, and a highlight where the light, the camera and the normal
  // line up.
  const Frame metal{"pbr_metal"};
  if (!metal.ok()) {
    return;
  }
  metal.expect(kCenter, kCenter, scaled(kOrange, kSunPbr * kMetalShine));
}

TEST_CASE("model3d: a texture of the model shows on its faces") {
  const Frame frame{"textured"};
  if (!frame.ok()) {
    return;
  }
  // The front face covers pixels 24 to 40; the texture is red, green / blue, white.
  frame.expect(kLeftQuarter, kLeftQuarter, {kFull, 0, 0});
  frame.expect(kRightQuarter, kLeftQuarter, {0, kFull, 0});
  frame.expect(kLeftQuarter, kRightQuarter, {0, 0, kFull});
  frame.expect(kRightQuarter, kRightQuarter, {kFull, kFull, kFull});
}

TEST_CASE("model3d: a game shader that includes mesh.hlsli draws a model") {
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build");
    return;
  }
  const Frame frame{"custom"};
  if (!frame.ok()) {
    return;
  }
  const std::array<int, 3> expected{
      static_cast<int>(std::lround((static_cast<float>(kOrange.at(0)) * (1 - kWhitenAmount)) +
                                   (kByteMax * kWhitenAmount))),
      static_cast<int>(std::lround((static_cast<float>(kOrange.at(1)) * (1 - kWhitenAmount)) +
                                   (kByteMax * kWhitenAmount))),
      static_cast<int>(std::lround((static_cast<float>(kOrange.at(2)) * (1 - kWhitenAmount)) +
                                   (kByteMax * kWhitenAmount)))};
  frame.expect(kCenter, kCenter, expected);
}

TEST_CASE("model3d: a skinned bar bent by its clip draws as bent, with the vertices uploaded") {
  // The camera looks at the tip joint at (0, 1, 0), 6.7 pixels to a unit at the front of the bar.
  // "bend" at its end (seek = 1) turns the top half to the left: the arm covers (x = -0.6,
  // y = 1), nothing is above the joint, and the lower half still stands.
  const Frame frame{"skinned_bent"};
  if (!frame.ok()) {
    return;
  }
  constexpr int kArmX = 28;      // x = -0.6
  constexpr int kJointRow = 32;  // y = 1
  constexpr int kAboveRow = 28;  // y = 1.5, above the bent arm
  constexpr int kBelowRow = 38;  // y = 0.04, in the lower half
  frame.expect(kArmX, kJointRow, kOrange);
  frame.expect(kCenter, kAboveRow, {0, 0, 0});
  frame.expect(kCenter, kBelowRow, kOrange);
}
