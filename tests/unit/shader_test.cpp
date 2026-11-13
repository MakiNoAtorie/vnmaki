// Game shaders (docs/SPEC.md sections 9 and 10), without a GPU: the run-time compiler, the
// script API that declares and sets shaders, uniform tweens and their snapshots.

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::Layer;
using vnmaki::RunState;
using vnmaki::Scene;

constexpr vnmaki::Size kVirtualSize{.width = 32, .height = 32};
constexpr float kHalf = 0.5F;
constexpr float kQuarter = 0.25F;
constexpr double kHalfSecond = 0.5;
constexpr std::uint32_t kSpirvMagic = 0x07230203;

// Two uniforms in one 16-byte row.
constexpr std::string_view kSepia = R"hlsl(#include "vnmaki.hlsli"

cbuffer SepiaUniforms : register(b0, space3) {
  float amount;
  float strength;
  float2 unused_padding;
};

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 color = sample_sprite(input) * input.color;
  return float4(color.rgb * (1.0 - amount * strength), color.a);
}
)hlsl";

// Line 5 uses a name that does not exist.
constexpr std::string_view kBroken = R"hlsl(#include "vnmaki.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 color = sample_sprite(input);
  return color * missing_name;
}
)hlsl";

bool compiler_missing() {
  if (vnmaki::shader_compiler_available()) {
    return false;
  }
  MESSAGE("no run-time shader compiler in this build or on this machine");
  return true;
}

class CommandRecorder final : public vnmaki::ScriptObserver {
 public:
  void on_scene_command(std::string_view command) override { commands.emplace_back(command); }

  std::vector<std::string> commands;
};

// A story whose script is `script`, with the sepia shader and one broken shader in the game.
struct Stage {
  explicit Stage(const std::string& script)
      : vfs(vnmaki::test::memory_vfs({
            {"scripts/main.lua", script},
            {"images/eileen.png", ""},
            {"images/lucy.png", ""},
            {"shaders/sepia.frag.hlsl", std::string{kSepia}},
            {"shaders/broken.frag.hlsl", std::string{kBroken}},
        })),
        scene(vfs, kVirtualSize),
        runtime(vfs, options()) {
    runtime.load_scripts();
    runtime.start("start");
  }

  [[nodiscard]] vnmaki::ScriptRuntimeOptions options() {
    vnmaki::ScriptRuntimeOptions result;
    result.observer = &recorder;
    result.scene_state = &scene;
    result.scene = &scene;
    return result;
  }

  [[nodiscard]] const vnmaki::DisplayProperties& properties(std::string_view tag) const {
    const vnmaki::Displayable* displayable = scene.find(tag);
    REQUIRE(displayable != nullptr);
    return displayable->properties();
  }

  vnmaki::Vfs vfs;
  CommandRecorder recorder;
  Scene scene;
  vnmaki::ScriptRuntime runtime;
};

// The error of a story whose script is `body` inside the `start` label (the body starts at line
// 3 of the script, below the sepia declaration).
std::string error_of(const std::string& body) {
  const Stage stage{
      "shader(\"sepia\", { \"amount\", \"strength\" })\nlabel(\"start\", function()\n" + body +
      "\nend)\n"};
  REQUIRE(stage.runtime.state() == RunState::kFailed);
  return stage.runtime.error();
}

constexpr std::string_view kShaded = R"(
shader("sepia", { "amount", "strength" })
label("start", function()
  show("eileen", { shader = "sepia", uniforms = { strength = 0.5 } })
  layer("overlay", { shader = "sepia", uniforms = { amount = 0.25 } })
  tween("eileen", { uniforms = { amount = 1 }, duration = 1 })
  tween("@layer.overlay", { uniforms = { amount = 1 }, duration = 1 })
  local choice = menu { "stay", "go" }
  narrate("end")
end)
)";

}  // namespace

TEST_CASE("shader compiler: a fragment shader compiles to every format") {
  if (compiler_missing()) {
    return;
  }
  const vnmaki::CompiledShader shader =
      vnmaki::compile_fragment_shader("shaders/sepia.frag.hlsl", kSepia);
  CHECK(shader.samplers == vnmaki::kMaxShaderTextures);  // vnmaki.hlsli declares every slot
  CHECK(shader.uniform_buffers == 1);
  REQUIRE(shader.spirv.size() > sizeof(kSpirvMagic));
  CHECK(shader.spirv.at(0) == (kSpirvMagic & 0xFF));
  CHECK_FALSE(shader.msl.empty());
#ifndef __APPLE__
  CHECK_FALSE(shader.dxil.empty());
#endif
  CHECK(shader.bytecode().spirv.size() == shader.spirv.size());
}

TEST_CASE("shader compiler: an error names the file, the line and the message") {
  if (compiler_missing()) {
    return;
  }
  try {
    static_cast<void>(vnmaki::compile_fragment_shader("shaders/broken.frag.hlsl", kBroken));
    FAIL("the shader should not compile");
  } catch (const vnmaki::ShaderCompileError& error) {
    const std::string message = error.what();
    INFO(message);
    CHECK(message.find("shaders/broken.frag.hlsl:5") != std::string::npos);
    CHECK(message.find("missing_name") != std::string::npos);
  }
}

TEST_CASE("shader: show sets a shader and uniforms and tween changes them") {
  if (compiler_missing()) {
    return;
  }
  Stage stage{std::string{kShaded}};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(stage.recorder.commands ==
        std::vector<std::string>{"show eileen shader sepia", "layer overlay shader sepia",
                                 "tween eileen", "tween @layer.overlay"});

  const vnmaki::DisplayProperties& eileen = stage.properties("eileen");
  CHECK(eileen.shader == 1);
  CHECK(eileen.uniforms.at(0) == 0);
  CHECK(eileen.uniforms.at(1) == kHalf);
  CHECK(stage.scene.layer_properties(Layer::kOverlay).uniforms.at(0) == kQuarter);

  stage.scene.update(kHalfSecond);
  CHECK(eileen.uniforms.at(0) == kHalf);
  CHECK(eileen.uniforms.at(1) == kHalf);
  CHECK(stage.scene.layer_properties(Layer::kOverlay).uniforms.at(0) == 0.625F);
  stage.scene.update(kHalfSecond);
  CHECK(eileen.uniforms.at(0) == 1);
  CHECK(stage.scene.layer_properties(Layer::kOverlay).uniforms.at(0) == 1);
}

TEST_CASE("shader: a show without a shader keeps the shader and the uniforms") {
  if (compiler_missing()) {
    return;
  }
  Stage stage{R"(
shader("sepia", { "amount", "strength" })
label("start", function()
  show("eileen", { shader = "sepia", uniforms = { amount = 0.5 } })
  show("eileen", { x = 0.5 })
  show("lucy", { shader = "sepia" })
  show("lucy", { shader = "" })
  narrate("end")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  CHECK(stage.properties("eileen").shader == 1);
  CHECK(stage.properties("eileen").uniforms.at(0) == kHalf);
  CHECK(stage.properties("lucy").shader == 0);
}

TEST_CASE("shader: mistakes are script errors at the calling line") {
  if (compiler_missing()) {
    return;
  }
  SUBCASE("a shader that is not declared") {
    CHECK(error_of("  show(\"eileen\", { shader = \"other\" })") ==
          "scripts/main.lua:3: shader 'other' is not declared (use shader(name, { uniforms }) "
          "first)");
  }
  SUBCASE("a uniform that the shader does not declare") {
    CHECK(error_of("  show(\"eileen\", { shader = \"sepia\", uniforms = { bogus = 1 } })") ==
          "scripts/main.lua:3: shader 'sepia' has no uniform 'bogus'");
  }
  SUBCASE("uniforms without a shader") {
    CHECK(error_of("  show(\"eileen\", { uniforms = { amount = 1 } })") ==
          "scripts/main.lua:3: uniforms need a shader");
  }
  SUBCASE("a tween of a uniform the displayable has not") {
    CHECK(error_of("  show(\"eileen\")\n  tween(\"eileen\", { uniforms = { amount = 1 } })") ==
          "scripts/main.lua:4: 'eileen' has no uniform 'amount' (set a shader that declares it)");
  }
  SUBCASE("a layer that does not exist") {
    CHECK(error_of("  layer(\"front\", { shader = \"sepia\" })").find("unknown layer 'front'") !=
          std::string::npos);
  }
  SUBCASE("a uniform that is not a number") {
    CHECK(error_of("  show(\"eileen\", { shader = \"sepia\", uniforms = { amount = \"x\" } })")
              .find("uniforms must be a table of uniform names and numbers") != std::string::npos);
  }
}

TEST_CASE("shader: declarations are checked when they run") {
  if (compiler_missing()) {
    return;
  }
  const auto declared = [](const std::string& declaration) -> std::string {
    try {
      const Stage stage{declaration + "\nlabel(\"start\", function()\n  narrate(\"x\")\nend)\n"};
    } catch (const std::exception& error) {
      return error.what();  // A declaration runs while the scripts load.
    }
    return {};
  };
  CHECK(declared("shader(\"ghost\")") ==
        "scripts/main.lua:1: shader 'ghost' not found (no shaders/ghost.frag.hlsl)");
  CHECK(declared("shader(\"sepia\", { \"amount\", \"amount\" })")
            .find("declares the uniform 'amount' twice") != std::string::npos);
  CHECK(declared("shader(\"sepia\", { \"a\", \"b\", \"c\", \"d\", \"e\", \"f\", \"g\", \"h\", "
                 "\"i\", \"j\", \"k\", \"l\", \"m\", \"n\", \"o\", \"p\", \"q\" })")
            .find("declares more than 16 uniforms") != std::string::npos);
  CHECK(declared("shader(\"bad name\")").find("bad argument #1 to 'shader'") != std::string::npos);
  const std::string broken = declared("shader(\"broken\")");
  INFO(broken);
  CHECK(broken.find("shaders/broken.frag.hlsl:5") != std::string::npos);
  CHECK(declared("shader(\"sepia\", { \"amount\" })\nshader(\"sepia\", { \"amount\" })").empty());
}

TEST_CASE("shader: the snapshot keeps shaders and uniform values, and a replay restores them") {
  if (compiler_missing()) {
    return;
  }
  Stage live{std::string{kShaded}};
  REQUIRE_MESSAGE(live.runtime.state() == RunState::kWaiting, live.runtime.error());
  REQUIRE(live.runtime.label_snapshot() != nullptr);
  const vnmaki::LabelSnapshot snapshot = *live.runtime.label_snapshot();
  const std::vector<vnmaki::InputResult> inputs{live.runtime.input_log().begin(),
                                                live.runtime.input_log().end()};
  // The tweens run, so the snapshot keeps the end values.
  const std::vector<std::byte> captured = live.scene.capture();

  Stage fresh{std::string{kShaded}};
  fresh.runtime.replay(snapshot, inputs, 1);
  REQUIRE_MESSAGE(fresh.runtime.state() == RunState::kWaiting, fresh.runtime.error());
  CHECK(fresh.scene.capture() == captured);
  CHECK(fresh.properties("eileen").shader == 1);
  CHECK(fresh.properties("eileen").uniforms.at(0) == 1);
  CHECK(fresh.scene.layer_properties(Layer::kOverlay).shader == 1);
  CHECK(fresh.scene.layer_properties(Layer::kOverlay).uniforms.at(0) == 1);

  SUBCASE("restore() puts the values back") {
    Stage other{std::string{kShaded}};
    other.scene.update(1);
    other.scene.restore(captured);
    CHECK(other.properties("eileen").uniforms.at(0) == 1);
    CHECK(other.scene.layer_properties(Layer::kOverlay).uniforms.at(0) == 1);
  }
  SUBCASE("a scene without shaders has no shaders part") {
    Stage plain{"label(\"start\", function()\n  show(\"eileen\")\n  narrate(\"x\")\nend)\n"};
    REQUIRE_MESSAGE(plain.runtime.state() == RunState::kWaiting, plain.runtime.error());
    const std::vector<std::byte> bytes = plain.scene.capture();
    std::string text;
    for (const std::byte byte : bytes) {
      text.push_back(static_cast<char>(byte));
    }
    CHECK(text.find("shaders") == std::string::npos);
    CHECK(captured.size() > bytes.size());
  }
  SUBCASE("a truncated shaders part is rejected") {
    std::vector<std::byte> broken = captured;
    broken.pop_back();
    Stage other{"label(\"start\", function()\n  narrate(\"x\")\nend)\n"};
    CHECK_THROWS_AS(other.scene.restore(broken), std::invalid_argument);
  }
}

TEST_CASE("shader: a layered sprite takes a shader and uniforms like an image") {
  if (compiler_missing()) {
    return;
  }
  const std::string header =
      "shader(\"sepia\", { \"amount\", \"strength\" })\n"
      "layered(\"doll\", { order = { \"base\" }, base = \"eileen\" })\n";
  Stage stage{header + R"(
label("start", function()
  show("doll", { shader = "sepia", uniforms = { amount = 0.5 } })
  tween("doll", { uniforms = { strength = 1 }, duration = 1 })
  narrate("end")
end)
)"};
  REQUIRE_MESSAGE(stage.runtime.state() == RunState::kWaiting, stage.runtime.error());
  stage.scene.update(1);
  CHECK(stage.properties("doll").shader == 1);
  CHECK(stage.properties("doll").uniforms.at(0) == kHalf);
  CHECK(stage.properties("doll").uniforms.at(1) == 1);

  // The snapshot keeps it too.
  const std::vector<std::byte> captured = stage.scene.capture();
  Stage other{header + "label(\"start\", function()\n  narrate(\"x\")\nend)\n"};
  other.scene.restore(captured);
  CHECK(other.properties("doll").shader == 1);
  CHECK(other.properties("doll").uniforms.at(1) == 1);
}
