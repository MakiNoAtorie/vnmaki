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
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "engine/audio/voice_level.hpp"
#include "engine/model3d/animation.hpp"
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
  /// The clips that play, empty for a model that has not played one. State: the snapshot keeps the
  /// newest clip (see `settled`).
  ClipPlayer clips{};
  /// `lip_sync`: the morph target (by name) that follows the level of the voice with
  /// `lip_voice_tag`, or "" for none. State.
  std::string lip_morph{};
  std::string lip_voice_tag{};
  /// The voice level that the mouth follows now, from Scene::update. Not state: a restore and a
  /// replay leave the mouth closed.
  double lip_level = 0;
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
  /// The parsed file, read on first use, or null if it cannot be read. Use find_data() where the
  /// caller must not read files.
  [[nodiscard]] const ModelData* data(std::string_view path);
  /// The parsed file if check() or data() has read it.
  [[nodiscard]] const ModelData* find_data(std::string_view path) const;
  /// A mesh that a scene made for one model (the vertices of a skinned or morphed model change
  /// with the pose) and no longer needs. release_retired() destroys it, which only the renderer
  /// can.
  void retire(MeshId mesh) { retired_.push_back(mesh); }
  void release_retired(IRenderer& renderer);
  /// The shader builtin_shader() made, or the zero id before that.
  [[nodiscard]] ShaderId made_shader(ShadingStyle style) const {
    return shaders_.at(static_cast<std::size_t>(style));
  }

 private:
  const Vfs* vfs_;
  std::map<std::string, ModelData, std::less<>> parsed_;
  std::map<std::string, std::vector<GpuMesh>, std::less<>> uploaded_;
  std::set<std::string, std::less<>> failed_;  // files that data() could not read
  std::vector<MeshId> retired_;
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

  /// Starts the clip `clip` of the model `model` (see PlayOptions). Returns why it cannot: no
  /// such model, a clip the file does not have (when the file has been read), or an option out of
  /// range.
  [[nodiscard]] std::string play(std::string_view model, std::string_view clip,
                                 const PlayOptions& options);
  /// True while the model `model` (all models when empty) plays a clip that does not loop.
  [[nodiscard]] bool clip_busy(std::string_view model) const;
  /// Puts the clips of the model (all when empty) that do not loop at their end.
  void finish_model_clips(std::string_view model);
  /// Sets `Model3d::lip_level` of each model with a `lip_morph` from the voice levels, or to 0.
  void set_voice_level(const VoiceLevel* levels);

  /// Uploads the models and makes the render target. Throws ModelError and RenderError.
  void prepare(IRenderer& renderer);
  /// Draws the models into the target in a pass of its own (with a depth buffer), which begins
  /// and ends there: the caller starts its pass again afterwards.
  void render(IRenderer& renderer) const;

  /// Moves the clips of the models on.
  void update(double seconds) override;
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
  void update_instances(IRenderer& renderer);

  // The meshes of the models whose vertices change with the pose, for each model tag. Each model
  // has its own, because two models of one file may play different clips.
  struct Instance {
    std::string path{};
    std::vector<MeshId> meshes{};
  };
  std::map<std::string, Instance, std::less<>> instances_;
};

/// The pose of `model`, which has the file `data`: its clips, with the morph target of its lip sync
/// at the voice level (0 to 1). What the scene turns into vertices.
[[nodiscard]] ModelPose pose_of(const Model3d& model, const ModelData& data);

/// A new pose with the channel defaults of a model (scale 1), a light or the camera.
[[nodiscard]] DisplayProperties default_model_pose();
[[nodiscard]] DisplayProperties default_light_pose(LightKind kind);
[[nodiscard]] DisplayProperties default_camera_pose();

}  // namespace vnmaki
