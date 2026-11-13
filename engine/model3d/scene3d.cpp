#include "engine/model3d/scene3d.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine/model3d/animation.hpp"
#include "engine/render/builtin_shaders.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::array<std::string_view, 3> kStyleNames{"unlit", "toon", "pbr"};
constexpr std::array<std::string_view, 3> kLightKindNames{"directional", "point", "ambient"};
constexpr Color kTransparent{.r = 0, .g = 0, .b = 0, .a = 0};

constexpr float kDefaultCameraDistance = 5.0F;
constexpr float kDefaultFov = 60.0F;
constexpr float kDefaultAmbientIntensity = 0.2F;

// The channels of kModelChannels, kLightChannels and kCameraChannels by index.
constexpr std::size_t kX = 0;
constexpr std::size_t kZ = 2;
constexpr std::size_t kModelRotationX = 3;
constexpr std::size_t kModelRotationY = 4;
constexpr std::size_t kModelRotationZ = 5;
constexpr std::size_t kModelScale = 6;
constexpr std::size_t kLightRed = 3;
constexpr std::size_t kLightGreen = 4;
constexpr std::size_t kLightBlue = 5;
constexpr std::size_t kLightIntensity = 6;
constexpr std::size_t kCameraLookX = 3;
constexpr std::size_t kCameraFov = 6;

// The constant buffer of the built-in mesh shaders, as MeshGlobals in shaders/mesh.hlsli lays it
// out: every member is a float4.
struct LightData {
  std::array<float, 4> vector{};
  std::array<float, 4> color{};
};
struct MeshGlobals {
  std::array<float, 4> base_color{};
  std::array<float, 4> material{};
  std::array<float, 4> camera_position{};
  std::array<float, 4> ambient{};
  std::array<LightData, kMaxLights> lights{};
};
static_assert(sizeof(MeshGlobals) == sizeof(std::array<float, 4>) * (4 + (2 * kMaxLights)));

float finite_or(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

Vec3 vec3_of(const DisplayProperties& pose, std::size_t first) {
  return {.x = finite_or(pose.uniforms.at(first), 0),
          .y = finite_or(pose.uniforms.at(first + 1), 0),
          .z = finite_or(pose.uniforms.at(first + 2), 0)};
}

// The pose of "camera", a model or a light, as a pointer to const when `self` is const.
template <typename Self>
auto* find_pose(Self& self, std::string_view name) {
  using Pose =
      std::conditional_t<std::is_const_v<Self>, const DisplayProperties, DisplayProperties>;
  Pose* found = nullptr;
  if (name == kCameraName) {
    found = &self.camera();
  }
  for (auto& model : self.models()) {
    found = model.tag == name ? &model.properties : found;
  }
  for (auto& light : self.lights()) {
    found = light.name == name ? &light.properties : found;
  }
  return found;
}

}  // namespace

std::string_view style_name(ShadingStyle style) {
  return kStyleNames.at(static_cast<std::size_t>(style));
}

bool style_from_name(std::string_view name, ShadingStyle& style) {
  const auto found = std::ranges::find(kStyleNames, name);
  if (found == kStyleNames.end()) {
    return false;
  }
  style = static_cast<ShadingStyle>(found - kStyleNames.begin());
  return true;
}

std::string_view light_kind_name(LightKind kind) {
  return kLightKindNames.at(static_cast<std::size_t>(kind));
}

bool light_kind_from_name(std::string_view name, LightKind& kind) {
  const auto found = std::ranges::find(kLightKindNames, name);
  if (found == kLightKindNames.end()) {
    return false;
  }
  kind = static_cast<LightKind>(found - kLightKindNames.begin());
  return true;
}

DisplayProperties default_model_pose() {
  DisplayProperties pose;
  pose.uniforms.at(kModelScale) = 1;
  return pose;
}

DisplayProperties default_light_pose(LightKind kind) {
  DisplayProperties pose;
  if (kind == LightKind::kDirectional) {
    pose.uniforms.at(kZ) = -1;
  }
  pose.uniforms.at(kLightRed) = 1;
  pose.uniforms.at(kLightGreen) = 1;
  pose.uniforms.at(kLightBlue) = 1;
  pose.uniforms.at(kLightIntensity) = kind == LightKind::kAmbient ? kDefaultAmbientIntensity : 1;
  return pose;
}

DisplayProperties default_camera_pose() {
  DisplayProperties pose;
  pose.uniforms.at(kZ) = kDefaultCameraDistance;
  pose.uniforms.at(kCameraFov) = kDefaultFov;
  return pose;
}

// --- ModelLibrary ----------------------------------------------------------------------------

std::string ModelLibrary::check(std::string_view path) {
  if (parsed_.contains(path)) {
    return {};
  }
  failed_.erase(std::string{path});
  try {
    parsed_.emplace(std::string{path}, load_model(*vfs_, path));
  } catch (const ModelError& error) {
    return error.what();
  }
  return {};
}

const std::vector<ModelLibrary::GpuMesh>& ModelLibrary::upload(IRenderer& renderer,
                                                               std::string_view path) {
  if (const auto found = uploaded_.find(path); found != uploaded_.end()) {
    return found->second;
  }
  if (!parsed_.contains(path)) {
    parsed_.emplace(std::string{path}, load_model(*vfs_, path));
  }
  const ModelData& data = parsed_.find(path)->second;
  if (white_ == TextureId{}) {
    white_ = renderer.create_texture(Image{.size = {.width = 1, .height = 1},
                                           .pixels = {UINT8_MAX, UINT8_MAX, UINT8_MAX, UINT8_MAX}},
                                     TextureFilter::kLinear);
  }
  std::vector<TextureId> textures;
  textures.reserve(data.images.size());
  for (const Image& image : data.images) {
    textures.push_back(renderer.create_texture(image, TextureFilter::kLinear));
  }
  std::vector<GpuMesh> meshes;
  meshes.reserve(data.meshes.size());
  for (const ModelMesh& mesh : data.meshes) {
    const ModelMaterial& material = data.materials.at(mesh.material);
    meshes.push_back(GpuMesh{.mesh = renderer.create_mesh(mesh.vertices, mesh.indices),
                             .material = material,
                             .texture = material.image >= 0
                                            ? textures.at(static_cast<std::size_t>(material.image))
                                            : white_});
  }
  return uploaded_.emplace(std::string{path}, std::move(meshes)).first->second;
}

const ModelData* ModelLibrary::data(std::string_view path) {
  if (const ModelData* parsed = find_data(path); parsed != nullptr) {
    return parsed;
  }
  if (failed_.contains(path)) {
    return nullptr;
  }
  try {
    return &parsed_.emplace(std::string{path}, load_model(*vfs_, path)).first->second;
  } catch (const ModelError&) {
    failed_.emplace(path);  // upload() reports the reason
    return nullptr;
  }
}

const ModelData* ModelLibrary::find_data(std::string_view path) const {
  const auto found = parsed_.find(path);
  return found == parsed_.end() ? nullptr : &found->second;
}

void ModelLibrary::release_retired(IRenderer& renderer) {
  for (const MeshId mesh : retired_) {
    renderer.destroy_mesh(mesh);
  }
  retired_.clear();
}

const std::vector<ModelLibrary::GpuMesh>* ModelLibrary::find(std::string_view path) const {
  const auto found = uploaded_.find(path);
  return found == uploaded_.end() ? nullptr : &found->second;
}

ShaderId ModelLibrary::builtin_shader(IRenderer& renderer, ShadingStyle style) {
  ShaderId& made = shaders_.at(static_cast<std::size_t>(style));
  if (made == ShaderId{}) {
    switch (style) {
      case ShadingStyle::kUnlit:
        made = renderer.create_shader(shaders::mesh_unlit_frag());
        break;
      case ShadingStyle::kToon:
        made = renderer.create_shader(shaders::mesh_toon_frag());
        break;
      case ShadingStyle::kPbr:
        made = renderer.create_shader(shaders::mesh_pbr_frag());
        break;
    }
  }
  return made;
}

ModelPose pose_of(const Model3d& model, const ModelData& data) {
  ModelPose pose = evaluate_pose(data, model.clips);
  if (!model.lip_morph.empty()) {
    set_morph_weight(data, pose, model.lip_morph,
                     static_cast<float>(std::clamp(model.lip_level, 0.0, 1.0)));
  }
  return pose;
}

// --- Scene3dDisplayable ----------------------------------------------------------------------

Scene3dDisplayable::Scene3dDisplayable(Size size, std::shared_ptr<ModelLibrary> library,
                                       std::shared_ptr<std::vector<TextureId>> retired)
    : size_(size),
      library_(std::move(library)),
      retired_(std::move(retired)),
      camera_(default_camera_pose()) {}

Scene3dDisplayable::~Scene3dDisplayable() {
  try {
    for (const auto& [tag, instance] : instances_) {
      for (const MeshId mesh : instance.meshes) {
        library_->retire(mesh);
      }
    }
    if (target_ != TextureId{}) {
      retired_->push_back(target_);
    }
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Out of memory: the target and the meshes stay on the GPU until the renderer goes.
  }
}

DisplayProperties* Scene3dDisplayable::object(std::string_view name) {
  return find_pose(*this, name);
}

const DisplayProperties* Scene3dDisplayable::object(std::string_view name) const {
  return find_pose(*this, name);
}

std::optional<std::size_t> Scene3dDisplayable::channel(
    std::string_view name, std::string_view channel,
    const std::vector<std::string>& shader_uniforms) const {
  const auto index_in =
      [&](const std::array<std::string_view, kPoseChannels>& names) -> std::optional<std::size_t> {
    const auto found = std::ranges::find(names, channel);
    if (found == names.end()) {
      return std::nullopt;
    }
    return static_cast<std::size_t>(found - names.begin());
  };
  if (name == kCameraName) {
    return index_in(kCameraChannels);
  }
  for (const Model3d& model : models_) {
    if (model.tag == name) {
      if (const std::optional<std::size_t> own = index_in(kModelChannels)) {
        return own;
      }
      const auto found = std::ranges::find(shader_uniforms, channel);
      const auto index = static_cast<std::size_t>(std::distance(shader_uniforms.begin(), found));
      if (found == shader_uniforms.end() || index >= kMaxModelShaderUniforms) {
        return std::nullopt;
      }
      return kPoseChannels + index;
    }
  }
  for (const Light3d& light : lights_) {
    if (light.name == name) {
      return index_in(kLightChannels);
    }
  }
  return std::nullopt;
}

std::string Scene3dDisplayable::play(std::string_view model, std::string_view clip,
                                     const PlayOptions& options) {
  const auto found = std::ranges::find(models_, model, &Model3d::tag);
  if (found == models_.end()) {
    return "this scene has no model '" + std::string{model} + "'";
  }
  if (!(options.fade >= 0 && options.fade <= kMaxFadeSeconds)) {
    return "option 'fade' must be a number from 0 to " + std::to_string(kMaxFadeSeconds);
  }
  if (!(options.speed > 0 && options.speed <= kMaxClipSpeed)) {
    return "option 'speed' must be a number above 0 and up to " + std::to_string(kMaxClipSpeed);
  }
  if (!(options.seek >= 0 && options.seek <= kMaxSeekSeconds)) {
    return "option 'seek' must be a number from 0 to " + std::to_string(kMaxSeekSeconds);
  }
  // A replay does not read the file (see Scene::set_model_3d), so it cannot check the name.
  if (const ModelData* data = library_->find_data(found->path);
      data != nullptr && find_clip(*data, clip) == nullptr) {
    std::string names;
    for (const ModelClip& known : data->clips) {
      names += (names.empty() ? "" : ", ") + known.name;
    }
    return "model '" + std::string{model} + "' has no clip '" + std::string{clip} + "' (" +
           (names.empty() ? std::string{"it has no clips"} : "its clips are " + names) + ")";
  }
  start_clip(found->clips, std::string{clip}, options);
  return {};
}

bool Scene3dDisplayable::clip_busy(std::string_view model) const {
  return std::ranges::any_of(models_, [&](const Model3d& candidate) {
    const ModelData* data = library_->find_data(candidate.path);
    return (model.empty() || candidate.tag == model) && data != nullptr &&
           clips_busy(candidate.clips, *data);
  });
}

void Scene3dDisplayable::finish_model_clips(std::string_view model) {
  for (Model3d& candidate : models_) {
    if (model.empty() || candidate.tag == model) {
      finish_clips(candidate.clips);
    }
  }
}

void Scene3dDisplayable::set_voice_level(const VoiceLevel* levels) {
  for (Model3d& model : models_) {
    model.lip_level = levels != nullptr && !model.lip_morph.empty()
                          ? levels->voice_level(model.lip_voice_tag).value_or(0)
                          : 0;
  }
}

void Scene3dDisplayable::update(double seconds) {
  for (Model3d& model : models_) {
    if (model.clips.layers.empty()) {
      continue;
    }
    if (const ModelData* data = library_->data(model.path)) {
      advance_clips(model.clips, *data, seconds);
    }
  }
}

bool Scene3dDisplayable::remove(std::string_view name) {
  if (const auto instance = instances_.find(name); instance != instances_.end()) {
    for (const MeshId mesh : instance->second.meshes) {
      library_->retire(mesh);
    }
    instances_.erase(instance);
  }
  const std::size_t before = models_.size() + lights_.size();
  std::erase_if(models_, [name](const Model3d& model) { return model.tag == name; });
  std::erase_if(lights_, [name](const Light3d& light) { return light.name == name; });
  return models_.size() + lights_.size() != before;
}

void Scene3dDisplayable::prepare(IRenderer& renderer) {
  if (target_ == TextureId{}) {
    target_ = renderer.create_render_target(size_, TextureFilter::kLinear);
  }
  for (const Model3d& model : models_) {
    static_cast<void>(library_->upload(renderer, model.path));
  }
  update_instances(renderer);
  for (const ShadingStyle style : {ShadingStyle::kUnlit, ShadingStyle::kToon, ShadingStyle::kPbr}) {
    if (std::ranges::any_of(models_, [style](const Model3d& model) {
          return model.properties.shader == 0 && model.style == style;
        })) {
      static_cast<void>(library_->builtin_shader(renderer, style));
    }
  }
}

// Gives each model whose vertices change with the pose (a skin or a morph target) meshes of its
// own, and puts the vertices of the pose into them. They go to the GPU on each call.
void Scene3dDisplayable::update_instances(IRenderer& renderer) {
  for (auto instance = instances_.begin(); instance != instances_.end();) {
    const auto model = std::ranges::find(models_, instance->first, &Model3d::tag);
    if (model != models_.end() && model->path == instance->second.path) {
      ++instance;
      continue;
    }
    for (const MeshId mesh : instance->second.meshes) {
      library_->retire(mesh);
    }
    instance = instances_.erase(instance);
  }
  std::vector<MeshVertex> vertices;
  for (const Model3d& model : models_) {
    const ModelData* data = library_->data(model.path);
    if (data == nullptr || !is_dynamic(*data)) {
      continue;
    }
    const ModelPose pose = pose_of(model, *data);
    const std::vector<std::vector<Mat4>> skins = skin_matrices(*data, pose);
    const auto found = instances_.find(model.tag);
    std::vector<MeshId> created;
    for (std::size_t i = 0; i < data->meshes.size(); ++i) {
      const ModelMesh& mesh = data->meshes.at(i);
      pose_vertices(mesh, pose, skins, vertices);
      if (found == instances_.end()) {
        created.push_back(renderer.create_mesh(vertices, mesh.indices));
      } else {
        renderer.update_mesh(found->second.meshes.at(i), vertices);
      }
    }
    if (found == instances_.end()) {
      instances_.emplace(model.tag, Instance{.path = model.path, .meshes = std::move(created)});
    }
  }
}

void Scene3dDisplayable::render(IRenderer& renderer) const {
  if (target_ == TextureId{}) {
    return;
  }
  renderer.begin_pass({.target = target_, .clear = kTransparent, .depth = true});

  const Vec3 eye = vec3_of(camera_, kX);
  const Vec3 look = vec3_of(camera_, kCameraLookX);
  const float fov =
      std::clamp(finite_or(camera_.uniforms.at(kCameraFov), kDefaultFov), kMinFov, kMaxFov);
  const float aspect = static_cast<float>(size_.width) / static_cast<float>(size_.height);
  const Mat4 view_projection = multiply(perspective(fov, aspect), look_at(eye, look));

  MeshGlobals globals;
  globals.camera_position = {eye.x, eye.y, eye.z, 1};
  std::size_t used = 0;
  for (const Light3d& light : lights_) {
    const DisplayProperties& pose = light.properties;
    const float intensity = finite_or(pose.uniforms.at(kLightIntensity), 0);
    const std::array<float, 3> color{finite_or(pose.uniforms.at(kLightRed), 0) * intensity,
                                     finite_or(pose.uniforms.at(kLightGreen), 0) * intensity,
                                     finite_or(pose.uniforms.at(kLightBlue), 0) * intensity};
    if (light.kind == LightKind::kAmbient) {
      for (std::size_t i = 0; i < color.size(); ++i) {
        globals.ambient.at(i) += color.at(i);
      }
    } else if (used < kMaxLights) {
      const Vec3 vector = vec3_of(pose, kX);
      globals.lights.at(used++) = LightData{
          .vector = {vector.x, vector.y, vector.z, light.kind == LightKind::kPoint ? 1.0F : 0.0F},
          .color = {color.at(0), color.at(1), color.at(2), 0}};
    }
  }

  // The constant buffer of a model with a game shader: the globals, then the shader's floats.
  std::array<std::byte, sizeof(MeshGlobals) + (sizeof(float) * kMaxUniforms)> bytes{};
  for (const Model3d& model : models_) {
    const std::vector<ModelLibrary::GpuMesh>* meshes = library_->find(model.path);
    if (meshes == nullptr) {
      continue;
    }
    const DisplayProperties& pose = model.properties;
    const float scale = finite_or(pose.uniforms.at(kModelScale), 1);
    const Mat4 world = model_matrix(vec3_of(pose, kX),
                                    {.x = finite_or(pose.uniforms.at(kModelRotationX), 0),
                                     .y = finite_or(pose.uniforms.at(kModelRotationY), 0),
                                     .z = finite_or(pose.uniforms.at(kModelRotationZ), 0)},
                                    scale);
    const bool custom = pose.shader != 0 && model.gpu_shader != ShaderId{};
    const ShaderId shader = custom ? model.gpu_shader : library_->made_shader(model.style);
    if (shader == ShaderId{}) {
      continue;
    }
    const auto own = instances_.find(model.tag);
    const bool animated = own != instances_.end() && own->second.path == model.path &&
                          own->second.meshes.size() == meshes->size();
    for (std::size_t i = 0; i < meshes->size(); ++i) {
      const ModelLibrary::GpuMesh& mesh = meshes->at(i);
      globals.base_color = mesh.material.color;
      globals.material = {mesh.material.metallic, mesh.material.roughness, 0, 0};
      std::memcpy(bytes.data(), &globals, sizeof(globals));
      std::size_t size = sizeof(globals);
      if (custom) {
        // Only the uniforms after the model's own channels belong to the shader; the bytes after
        // them stay zero.
        const std::size_t floats = std::min(model.shader_floats, kMaxModelShaderUniforms);
        std::memcpy(std::span{bytes}.subspan(size).data(),
                    std::span{pose.uniforms}.subspan(kPoseChannels).data(), floats * sizeof(float));
        size += model.shader_floats * sizeof(float);
      }
      renderer.draw_mesh(MeshDraw{.mesh = animated ? own->second.meshes.at(i) : mesh.mesh,
                                  .view_projection = view_projection,
                                  .model = world,
                                  .texture = mesh.texture,
                                  .shader = shader,
                                  .uniforms = std::span{bytes}.first(size)});
    }
  }
}

void Scene3dDisplayable::draw(IRenderer& renderer) const {
  if (target_ == TextureId{}) {
    return;
  }
  Color tint = properties().tint;
  tint.a *= properties().alpha;
  renderer.draw(
      Quad{.corners = place_corners(properties(), size_), .uv = properties().crop, .tint = tint},
      draw_state(target_));
}

RectF Scene3dDisplayable::bounds() const {
  return bounding_box(place_corners(properties(), size_));
}

}  // namespace vnmaki
