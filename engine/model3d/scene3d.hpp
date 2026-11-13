#pragma once

// 3D scenes (docs/SPEC.md sections 9 and 12): a camera, lights and glTF models that draw into a
// render target with a depth buffer. A Scene3dDisplayable is a displayable on a layer (the "3d"
// layer by default), so the 3D picture mixes with the 2D layers, and its alpha, tint, shader and
// position tween like those of an image.
//
// The camera, each model and each light have a pose: a few numbers (the channels below) kept in
// the `uniforms` of a DisplayProperties. The animation system tweens them by the tag
// "@3d.<scene>.<name>" (name: "camera", a model's tag or a light's name), so `wait` works on them.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/model3d/math3d.hpp"
#include "engine/model3d/model.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

class Vfs;

/// The tags of the tweens of 3D objects start with this: "@3d.<scene>.<name>".
inline constexpr std::string_view kScene3dTagPrefix = "@3d.";
/// The name of a scene's camera in a tween tag. A model or a light cannot have it.
inline constexpr std::string_view kCameraName = "camera";

/// The channels (indices in DisplayProperties::uniforms, which the timeline tweens by name) of a
/// model, a light and the camera. A model with a game shader has the shader's uniforms after its
/// own channels, in the order the shader declared them (at most 9).
inline constexpr std::size_t kPoseChannels = 7;
inline constexpr std::array<std::string_view, kPoseChannels> kModelChannels{
    "x", "y", "z", "rx", "ry", "rz", "scale"};
inline constexpr std::array<std::string_view, kPoseChannels> kLightChannels{
    "x", "y", "z", "r", "g", "b", "intensity"};
inline constexpr std::array<std::string_view, kPoseChannels> kCameraChannels{"x",  "y",  "z",  "lx",
                                                                             "ly", "lz", "fov"};
/// The first of the three channels that hold a point (position, direction), a rotation in
/// degrees (a model), a look-at point (the camera) or a colour (a light). The last channel is a
/// model's scale, a light's intensity or the camera's field of view.
inline constexpr std::size_t kPointChannel = 0;
inline constexpr std::size_t kSecondChannels = 3;
inline constexpr std::size_t kLastChannel = 6;
/// A model's shader may declare this many uniforms.
inline constexpr std::size_t kMaxModelShaderUniforms = kMaxUniforms - kPoseChannels;
/// Lights that are not ambient. More are an error.
inline constexpr std::size_t kMaxLights = 4;
/// The most models and the most lights a scene has.
inline constexpr std::size_t kMaxSceneObjects = 256;

/// How a model is lit when it has no game shader.
enum class ShadingStyle : std::uint8_t { kUnlit, kToon, kPbr };

/// The names scripts use: "unlit", "toon" and "pbr".
[[nodiscard]] std::string_view style_name(ShadingStyle style);
[[nodiscard]] bool style_from_name(std::string_view name, ShadingStyle& style);

enum class LightKind : std::uint8_t {
  kDirectional,  ///< x, y, z is the direction the light travels.
  kPoint,        ///< x, y, z is the position.
  kAmbient,      ///< Lights everything alike; x, y, z mean nothing.
};

/// The names scripts use: "directional", "point" and "ambient".
[[nodiscard]] std::string_view light_kind_name(LightKind kind);
[[nodiscard]] bool light_kind_from_name(std::string_view name, LightKind& kind);

struct Model3d {
  std::string tag{};
  /// The Vfs path of the .gltf or .glb file.
  std::string path{};
  ShadingStyle style = ShadingStyle::kUnlit;
  /// `properties.shader` (the number Scene gives a game shader) replaces the style when it is
  /// not 0; the uniforms are the channels above.
  DisplayProperties properties{};
  /// Set by Scene::prepare: the GPU shader and the floats of its constant buffer.
  ShaderId gpu_shader{};
  std::size_t shader_floats = 0;
};

struct Light3d {
  std::string name{};
  LightKind kind = LightKind::kDirectional;
  DisplayProperties properties{};
};

/// The models that 3D scenes use, kept for the life of the scene: the parsed files and, once a
/// renderer is given, their meshes and textures on the GPU. Nothing is freed until the renderer
/// goes away (ponytail: no eviction; a game has a handful of models).
class ModelLibrary {
 public:
  /// A mesh on the GPU with its material.
  struct GpuMesh {
    MeshId mesh{};
    ModelMaterial material{};
    TextureId texture{};
  };

  /// `vfs` must outlive the library.
  explicit ModelLibrary(const Vfs& vfs) : vfs_(&vfs) {}

  /// Checks that `path` is a model that loads, keeping it for later. Returns why not, or an empty
  /// string.
  [[nodiscard]] std::string check(std::string_view path);
  /// The meshes of `path` on the GPU, uploading them on first use. Throws ModelError and
  /// RenderError. The renderer must be the same one every time.
  [[nodiscard]] const std::vector<GpuMesh>& upload(IRenderer& renderer, std::string_view path);
  /// What upload() returned, or null before the first upload.
  [[nodiscard]] const std::vector<GpuMesh>* find(std::string_view path) const;
  /// The built-in fragment shader of a style, made on first use.
  [[nodiscard]] ShaderId builtin_shader(IRenderer& renderer, ShadingStyle style);
  /// The shader builtin_shader() made, or the zero id before that.
  [[nodiscard]] ShaderId made_shader(ShadingStyle style) const {
    return shaders_.at(static_cast<std::size_t>(style));
  }

 private:
  const Vfs* vfs_;
  std::map<std::string, ModelData, std::less<>> parsed_;
  std::map<std::string, std::vector<GpuMesh>, std::less<>> uploaded_;
  std::array<ShaderId, 3> shaders_{};
  TextureId white_{};
};

/// A scene of 3D models drawn into a target of its own size (the virtual resolution).
class Scene3dDisplayable final : public Displayable {
 public:
  /// `retired` collects the render target of a displayable that goes away, for Scene::prepare()
  /// to destroy: only the renderer can.
  Scene3dDisplayable(Size size, std::shared_ptr<ModelLibrary> library,
                     std::shared_ptr<std::vector<TextureId>> retired);
  Scene3dDisplayable(const Scene3dDisplayable&) = delete;
  Scene3dDisplayable& operator=(const Scene3dDisplayable&) = delete;
  Scene3dDisplayable(Scene3dDisplayable&&) = delete;
  Scene3dDisplayable& operator=(Scene3dDisplayable&&) = delete;
  ~Scene3dDisplayable() override;

  /// The camera's pose, channels kCameraChannels. It starts at (0, 0, 5) looking at the origin
  /// with a field of view of 60 degrees.
  [[nodiscard]] DisplayProperties& camera() { return camera_; }
  [[nodiscard]] const DisplayProperties& camera() const { return camera_; }
  [[nodiscard]] std::vector<Model3d>& models() { return models_; }
  [[nodiscard]] const std::vector<Model3d>& models() const { return models_; }
  [[nodiscard]] std::vector<Light3d>& lights() { return lights_; }
  [[nodiscard]] const std::vector<Light3d>& lights() const { return lights_; }

  /// The pose of "camera", a model or a light by name, or null.
  [[nodiscard]] DisplayProperties* object(std::string_view name);
  [[nodiscard]] const DisplayProperties* object(std::string_view name) const;
  /// The index of channel `channel` of the object `name` (the model's game shader declares
  /// `shader_uniforms`), or nothing.
  [[nodiscard]] std::optional<std::size_t> channel(
      std::string_view name, std::string_view channel,
      const std::vector<std::string>& shader_uniforms) const;
  /// Removes a model or a light. Returns false if there is none of that name.
  bool remove(std::string_view name);

  /// Uploads the models and makes the render target. Throws ModelError and RenderError.
  void prepare(IRenderer& renderer);
  /// Draws the models into the target in a pass of its own (with a depth buffer), which begins
  /// and ends there: the caller starts its pass again afterwards.
  void render(IRenderer& renderer) const;

  void update(double /*seconds*/) override {}
  /// Draws the picture that render() made, over the virtual screen, like an image.
  void draw(IRenderer& renderer) const override;
  [[nodiscard]] RectF bounds() const override;

 private:
  Size size_;
  std::shared_ptr<ModelLibrary> library_;
  std::shared_ptr<std::vector<TextureId>> retired_;
  DisplayProperties camera_{};
  std::vector<Model3d> models_;
  std::vector<Light3d> lights_;
  TextureId target_{};
};

/// A new pose with the channel defaults of a model (scale 1), a light or the camera.
[[nodiscard]] DisplayProperties default_model_pose();
[[nodiscard]] DisplayProperties default_light_pose(LightKind kind);
[[nodiscard]] DisplayProperties default_camera_pose();

}  // namespace vnmaki
