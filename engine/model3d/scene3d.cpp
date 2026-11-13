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

// --- Scene3dDisplayable ----------------------------------------------------------------------

Scene3dDisplayable::Scene3dDisplayable(Size size, std::shared_ptr<ModelLibrary> library,
                                       std::shared_ptr<std::vector<TextureId>> retired)
    : size_(size),
      library_(std::move(library)),
      retired_(std::move(retired)),
      camera_(default_camera_pose()) {}

Scene3dDisplayable::~Scene3dDisplayable() {
  if (target_ == TextureId{}) {
    return;
  }
  try {
    retired_->push_back(target_);
  } catch (...) {  // NOLINT(bugprone-empty-catch)
    // Out of memory: the target stays on the GPU until the renderer goes.
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

bool Scene3dDisplayable::remove(std::string_view name) {
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
  for (const ShadingStyle style : {ShadingStyle::kUnlit, ShadingStyle::kToon, ShadingStyle::kPbr}) {
    if (std::ranges::any_of(models_, [style](const Model3d& model) {
          return model.properties.shader == 0 && model.style == style;
        })) {
      static_cast<void>(library_->builtin_shader(renderer, style));
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
    for (const ModelLibrary::GpuMesh& mesh : *meshes) {
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
      renderer.draw_mesh(MeshDraw{.mesh = mesh.mesh,
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
