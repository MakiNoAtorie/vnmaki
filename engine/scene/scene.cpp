#include "engine/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/anim/easing.hpp"
#include "engine/anim/timeline.hpp"
#include "engine/core/byte_stream.hpp"
#include "engine/render/image.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::array<std::string_view, kLayerOrder.size()> kLayerNames{
    "background", "master", "characters", "3d", "overlay", "ui", "transient",
};

// The image-name convention: image name N is the file "images/N.png", or "images/N.jpg" when
// there is no such PNG. A name may hold "/": "bg/classroom" is "images/bg/classroom.png".
constexpr std::string_view kImageDirectory = "images/";
constexpr std::array<std::string_view, 2> kImageExtensions{".png", ".jpg"};

std::string image_path(std::string_view name, std::string_view extension) {
  std::string path{kImageDirectory};
  path += name;
  path += extension;
  return path;
}

std::optional<std::string> resolve_image(const Vfs& vfs, std::string_view name) {
  for (const std::string_view extension : kImageExtensions) {
    std::string path = image_path(name, extension);
    if (vfs.exists(path)) {
      return path;
    }
  }
  return std::nullopt;
}

std::string missing_image(std::string_view name) {
  std::string message = "image '";
  message += name;
  message += "' not found (no ";
  message += image_path(name, kImageExtensions.front());
  message += " or ";
  message += image_path(name, kImageExtensions.back());
  message += ")";
  return message;
}

// A named position: how far across the virtual width the displayable stands.
constexpr float kLeft = 0.25F;
constexpr float kCenter = 0.5F;
constexpr float kRight = 0.75F;
constexpr PointF kBottomCenter{.x = 0.5F, .y = 1.0F};

std::optional<float> named_position(std::string_view name) {
  if (name == "left") {
    return kLeft;
  }
  if (name == "center") {
    return kCenter;
  }
  if (name == "right") {
    return kRight;
  }
  return std::nullopt;
}

// A coordinate a script gave as a fraction of `extent`, or nothing when it is not a number in
// range (so the conversion to float is always defined).
std::optional<float> coordinate(double fraction, float extent) {
  if (!(std::abs(fraction) <= kMaxPositionFraction)) {
    return std::nullopt;
  }
  return static_cast<float>(fraction) * extent;
}

std::string out_of_range(std::string_view option) {
  std::string message = "option '";
  message += option;
  message += "' must be a number from -1000000 to 1000000";
  return message;
}

std::string quoted(std::string_view prefix, std::string_view name, std::string_view suffix) {
  std::string message{prefix};
  message += name;
  message += suffix;
  return message;
}

// --- Serialized form -------------------------------------------------------------------------
// Little-endian. A u32 format version, a u32 count of parts, then for each part in the order of
// its name: the name (a u32 length and the bytes) and its bytes (a u32 length and the bytes).
//
// "displayables": a u32 count, then for each displayable (layer by layer, back to front): u8
// layer, tag, image path (each a u32 length and the bytes) and the properties as 16 f32:
// position, anchor, scale, rotation, alpha, tint, crop.
// "layered": a u32 count, then for each layered sprite (layer by layer, back to front): u8 layer,
// u32 index in the layer (counting the images), tag, definition name, the properties as for
// "displayables", a u32 count of attributes and each one's group and value.
// "tweens": Timeline::capture().
// "shaders", only when a shader is set: a u32 count of displayables, then for each: tag, shader
// name and the 16 f32 uniform values; then a u32 count of layers, then for each: u8 layer, shader
// name and the 16 f32 uniform values.

constexpr std::uint32_t kFormatVersion = 2;
constexpr std::string_view kDisplayablesPart = "displayables";
constexpr std::string_view kLayeredPart = "layered";
constexpr std::string_view kTweensPart = "tweens";
constexpr std::string_view kShadersPart = "shaders";

constexpr std::string_view kShaderDirectory = "shaders/";
constexpr std::string_view kShaderExtension = ".frag.hlsl";
// HLSL packs a constant buffer in rows of four floats.
constexpr std::size_t kFloatsPerRow = 4;
constexpr Color kTransparent{.r = 0, .g = 0, .b = 0, .a = 0};

class Writer : public ByteWriter {
 public:
  void point(PointF value) {
    f32(value.x);
    f32(value.y);
  }
  void properties(const DisplayProperties& value) {
    point(value.position);
    point(value.anchor);
    point(value.scale);
    f32(value.rotation);
    f32(value.alpha);
    f32(value.tint.r);
    f32(value.tint.g);
    f32(value.tint.b);
    f32(value.tint.a);
    f32(value.crop.x);
    f32(value.crop.y);
    f32(value.crop.width);
    f32(value.crop.height);
  }
  void uniforms(const std::array<float, kMaxUniforms>& values) {
    for (const float value : values) {
      f32(value);
    }
  }
};

// Throws std::invalid_argument when the bytes end early.
class Reader : public ByteReader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : ByteReader(bytes, "scene state") {}

  PointF point() {
    const float x = f32();
    const float y = f32();
    return PointF{.x = x, .y = y};
  }
  DisplayProperties properties() {
    DisplayProperties value;
    value.position = point();
    value.anchor = point();
    value.scale = point();
    value.rotation = f32();
    value.alpha = f32();
    value.tint.r = f32();
    value.tint.g = f32();
    value.tint.b = f32();
    value.tint.a = f32();
    value.crop.x = f32();
    value.crop.y = f32();
    value.crop.width = f32();
    value.crop.height = f32();
    return value;
  }
  std::array<float, kMaxUniforms> uniforms() {
    std::array<float, kMaxUniforms> values{};
    for (float& value : values) {
      value = f32();
    }
    return values;
  }
};

// --- Tweens ----------------------------------------------------------------------------------

constexpr float kByteMax = 255.0F;
constexpr unsigned kRedShift = 16;
constexpr unsigned kGreenShift = 8;
constexpr std::uint32_t kByteMask = 0xFF;

float tint_channel(std::uint32_t rgb, unsigned shift) {
  return static_cast<float>((rgb >> shift) & kByteMask) / kByteMax;
}

std::string out_of_time(std::string_view option) {
  std::string message = "option '";
  message += option;
  message += "' must be a number from 0 to 86400";
  return message;
}

// The index of the uniform `name` of the shader that `tag` has, if it has one.
using UniformIndex =
    std::function<std::optional<std::size_t>(std::string_view tag, std::string_view name)>;

// Fills `node` from `request`, or returns why that cannot be done. Positions are fractions of the
// virtual resolution.
std::string tween_node(const TweenRequest& request, Size virtual_size,
                       const UniformIndex& uniform_index, TweenNode& node) {
  if (!(request.delay >= 0 && request.delay <= kMaxTweenSeconds)) {
    return out_of_time("delay");
  }
  node.delay = request.delay;
  node.loop = request.loop;
  if (request.kind != TweenRequest::Kind::kTween) {
    node.kind = request.kind == TweenRequest::Kind::kSequence ? TweenNode::Kind::kSequence
                                                              : TweenNode::Kind::kParallel;
    node.children.reserve(request.children.size());
    for (const TweenRequest& child : request.children) {
      if (std::string problem =
              tween_node(child, virtual_size, uniform_index, node.children.emplace_back());
          !problem.empty()) {
        return problem;
      }
    }
    return {};
  }

  node.tag = request.tag;
  node.yoyo = request.yoyo;
  if (!(request.duration >= 0 && request.duration <= kMaxTweenSeconds)) {
    return out_of_time("duration");
  }
  node.duration = request.duration;
  const std::optional<Ease> ease = ease_from_name(request.ease);
  if (!ease.has_value()) {
    return quoted("unknown easing '", request.ease,
                  "' (use linear, or in_, out_ or in_out_ followed by sine, quad, cubic, back or "
                  "bounce)");
  }
  node.ease = *ease;

  std::string problem;
  const auto set = [&](Channel channel, const std::optional<double>& value, std::string_view name,
                       float extent) {
    if (!value.has_value() || !problem.empty()) {
      return;
    }
    if (const std::optional<float> converted = coordinate(*value, extent)) {
      node.to.at(slot(channel)) = *converted;
    } else {
      problem = out_of_range(name);
    }
  };
  set(Channel::kX, request.x, "x", static_cast<float>(virtual_size.width));
  set(Channel::kY, request.y, "y", static_cast<float>(virtual_size.height));
  set(Channel::kAnchorX, request.anchor_x, "anchor.x", 1);
  set(Channel::kAnchorY, request.anchor_y, "anchor.y", 1);
  set(Channel::kScaleX, request.scale_x, "scale.x", 1);
  set(Channel::kScaleY, request.scale_y, "scale.y", 1);
  set(Channel::kRotation, request.rotation, "rotation", 1);
  set(Channel::kAlpha, request.alpha, "alpha", 1);
  set(Channel::kCropX, request.crop_x, "crop.x", 1);
  set(Channel::kCropY, request.crop_y, "crop.y", 1);
  set(Channel::kCropWidth, request.crop_width, "crop.width", 1);
  set(Channel::kCropHeight, request.crop_height, "crop.height", 1);
  if (request.tint.has_value()) {
    node.to.at(slot(Channel::kTintR)) = tint_channel(*request.tint, kRedShift);
    node.to.at(slot(Channel::kTintG)) = tint_channel(*request.tint, kGreenShift);
    node.to.at(slot(Channel::kTintB)) = tint_channel(*request.tint, 0);
  }
  for (const auto& [name, value] : request.uniforms) {
    const std::optional<std::size_t> index = uniform_index(request.tag, name);
    if (!index.has_value()) {
      return quoted("'", request.tag,
                    "' has no uniform '" + name + "' (set a shader that declares it)");
    }
    if (!(std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max()))) {
      return out_of_range("uniforms." + name);
    }
    node.to.at(uniform_slot(*index)) = static_cast<float>(value);
  }
  return problem;
}

}  // namespace

std::string_view layer_name(Layer layer) { return kLayerNames.at(static_cast<std::size_t>(layer)); }

std::optional<Layer> layer_from_name(std::string_view name) {
  const auto found = std::find(kLayerNames.begin(), kLayerNames.end(), name);
  if (found == kLayerNames.end()) {
    return std::nullopt;
  }
  return kLayerOrder.at(static_cast<std::size_t>(found - kLayerNames.begin()));
}

Scene::Scene(const Vfs& vfs, Size virtual_size) : vfs_(&vfs), virtual_size_(virtual_size) {}

std::vector<Scene::Entry>& Scene::entries(Layer layer) {
  return layers_.at(static_cast<std::size_t>(layer));
}

const std::vector<Scene::Entry>& Scene::entries(Layer layer) const {
  return layers_.at(static_cast<std::size_t>(layer));
}

std::optional<Scene::Slot> Scene::locate(std::string_view tag) const {
  for (const Layer layer : kLayerOrder) {
    const std::vector<Entry>& shown = entries(layer);
    for (std::size_t index = 0; index < shown.size(); ++index) {
      if (shown[index].tag == tag) {
        return Slot{.layer = layer, .index = index};
      }
    }
  }
  return std::nullopt;
}

Timeline::Lookup Scene::lookup() {
  return [this](std::string_view tag) { return properties_in(layers_, layer_state_, tag); };
}

DisplayProperties* Scene::properties_in(Layers& layers, LayerState& layer_state,
                                        std::string_view tag) {
  if (tag.starts_with(kLayerTagPrefix)) {
    const std::optional<Layer> layer = layer_from_name(tag.substr(kLayerTagPrefix.size()));
    return layer.has_value() ? &layer_state.at(static_cast<std::size_t>(*layer)) : nullptr;
  }
  for (std::vector<Entry>& shown : layers) {
    for (Entry& entry : shown) {
      if (entry.tag == tag) {
        return &entry.displayable->properties();
      }
    }
  }
  return nullptr;
}

const DisplayProperties* Scene::properties_of(std::string_view tag) const {
  if (tag.starts_with(kLayerTagPrefix)) {
    const std::optional<Layer> layer = layer_from_name(tag.substr(kLayerTagPrefix.size()));
    return layer.has_value() ? &layer_state_.at(static_cast<std::size_t>(*layer)) : nullptr;
  }
  const Displayable* shown = find(tag);
  return shown != nullptr ? &shown->properties() : nullptr;
}

std::optional<std::size_t> Scene::uniform_index(std::string_view tag, std::string_view name) const {
  const DisplayProperties* properties = properties_of(tag);
  const GameShader* shader = properties != nullptr ? shader_by_id(properties->shader) : nullptr;
  if (shader == nullptr) {
    return std::nullopt;
  }
  const std::vector<std::string>& names = shader->uniforms;
  const auto found = std::ranges::find(names, name);
  if (found == names.end()) {
    return std::nullopt;
  }
  return static_cast<std::size_t>(found - names.begin());
}

std::uint32_t Scene::shader_id(std::string_view name) const {
  const auto found = std::ranges::find(shaders_, name, &GameShader::name);
  return found == shaders_.end() ? 0 : static_cast<std::uint32_t>(found - shaders_.begin()) + 1;
}

const Scene::GameShader* Scene::shader_by_id(std::uint32_t id) const {
  return id >= 1 && id <= shaders_.size() ? &shaders_.at(id - 1) : nullptr;
}

std::string Scene::scene(std::string_view image) {
  std::optional<std::string> path = resolve_image(*vfs_, image);
  if (!path.has_value()) {
    return missing_image(image);
  }
  hide(kBackgroundTag);
  for (const Layer layer : {Layer::kBackground, Layer::kMaster, Layer::kCharacters}) {
    entries(layer).clear();
  }
  entries(Layer::kBackground)
      .push_back(Entry{.tag = std::string{kBackgroundTag},
                       .displayable = std::make_unique<ImageDisplayable>(std::move(*path))});
  timeline_.prune(lookup());
  return {};
}

std::string Scene::layered(const LayeredRequest& request) {
  if (request.name.empty()) {
    return "a layered sprite needs a name";
  }
  if (request.groups.empty()) {
    return "a layered sprite needs at least one group";
  }
  if (!(request.crossfade >= 0 && request.crossfade <= kMaxTweenSeconds)) {
    return "option 'crossfade' must be a number from 0 to 86400";
  }
  auto definition = std::make_shared<LayeredDefinition>();
  definition->name = request.name;
  definition->crossfade = request.crossfade;
  definition->groups.reserve(request.groups.size());
  for (const LayeredRequest::Group& group : request.groups) {
    LayeredDefinition::Group& added = definition->groups.emplace_back();
    added.name = group.name;
    added.values.reserve(group.values.size());
    for (const auto& [value, image] : group.values) {
      std::optional<std::string> path = resolve_image(*vfs_, image);
      if (!path.has_value()) {
        return missing_image(image);
      }
      added.values.push_back({.name = value, .path = std::move(*path)});
    }
    const bool fixed = added.values.size() == 1 && added.values.front().name.empty();
    const bool unnamed = std::ranges::any_of(
        added.values, [](const LayeredDefinition::Value& value) { return value.name.empty(); });
    if (added.name.empty() || added.values.empty() || (unnamed && !fixed)) {
      return quoted("the group '", added.name, "' needs a name and values with names");
    }
  }
  // The defaults are checked by choosing them in a sprite made without them.
  LayeredSprite probe{definition};
  if (std::string problem = probe.set_attributes(request.defaults, false); !problem.empty()) {
    return problem;
  }
  definition->defaults = request.defaults;
  definitions_.insert_or_assign(request.name, std::move(definition));
  return {};
}

std::string Scene::show(const ShowRequest& request) {
  const auto definition = definitions_.find(request.image);
  std::optional<std::string> path;
  if (definition == definitions_.end()) {
    path = resolve_image(*vfs_, request.image);
    if (!path.has_value()) {
      return missing_image(request.image);
    }
  }
  if (request.tag.starts_with(kLayerTagPrefix)) {
    return quoted("tags that start with '", kLayerTagPrefix, "' are reserved for layers");
  }
  const auto width = static_cast<float>(virtual_size_.width);
  const auto height = static_cast<float>(virtual_size_.height);
  const std::optional<Slot> shown = locate(request.tag);

  Layer layer = Layer::kCharacters;
  DisplayProperties properties;
  properties.position = PointF{.x = kCenter * width, .y = height};
  properties.anchor = kBottomCenter;
  if (shown.has_value()) {
    layer = shown->layer;
    properties = entries(layer).at(shown->index).displayable->properties();
  }
  if (request.layer.has_value()) {
    const std::optional<Layer> named = layer_from_name(*request.layer);
    if (!named.has_value()) {
      return quoted("unknown layer '", *request.layer,
                    "' (the layers are background, master, characters, 3d, overlay, ui and "
                    "transient)");
    }
    layer = *named;
  }
  if (request.at.has_value()) {
    const std::optional<float> across = named_position(*request.at);
    if (!across.has_value()) {
      return quoted("unknown position '", *request.at, "' (use left, center or right)");
    }
    properties.position = PointF{.x = *across * width, .y = height};
    properties.anchor = kBottomCenter;
  }
  if (request.x.has_value()) {
    const std::optional<float> x = coordinate(*request.x, width);
    if (!x.has_value()) {
      return out_of_range("x");
    }
    properties.position.x = *x;
  }
  if (request.y.has_value()) {
    const std::optional<float> y = coordinate(*request.y, height);
    if (!y.has_value()) {
      return out_of_range("y");
    }
    properties.position.y = *y;
  }

  std::unique_ptr<Displayable> displayable;
  if (definition != definitions_.end()) {
    // A sprite that is shown again keeps its groups, and the named ones change.
    const auto* before = shown.has_value()
                             ? dynamic_cast<const LayeredSprite*>(
                                   entries(shown->layer).at(shown->index).displayable.get())
                             : nullptr;
    const bool again = before != nullptr && before->name() == request.image;
    auto sprite = again ? std::make_unique<LayeredSprite>(*before)
                        : std::make_unique<LayeredSprite>(definition->second);
    if (std::string problem =
            sprite->set_attributes(request.attributes, again && !request.replaying);
        !problem.empty()) {
      return problem;
    }
    displayable = std::move(sprite);
  } else {
    displayable = std::make_unique<ImageDisplayable>(std::move(*path));
  }
  displayable->properties() = properties;
  if (shown.has_value() && shown->layer == layer) {
    entries(layer).at(shown->index).displayable = std::move(displayable);
    return {};
  }
  hide(request.tag);
  entries(layer).push_back(Entry{.tag = request.tag, .displayable = std::move(displayable)});
  return {};
}

void Scene::hide(std::string_view tag) {
  for (std::vector<Entry>& shown : layers_) {
    std::erase_if(shown, [tag](const Entry& entry) { return entry.tag == tag; });
  }
  timeline_.prune(lookup());
}

std::string Scene::tween(const TweenRequest& request) {
  TweenNode node;
  const UniformIndex uniform_index = [this](std::string_view tag, std::string_view name) {
    return this->uniform_index(tag, name);
  };
  if (std::string problem = tween_node(request, virtual_size_, uniform_index, node);
      !problem.empty()) {
    return problem;
  }
  return timeline_.start(node, lookup(), request.replaying);
}

void Scene::finish_tweens(std::string_view tag) { timeline_.finish(tag, lookup()); }

bool Scene::tweening(std::string_view tag) const { return timeline_.busy(tag); }

std::string Scene::declare_shader(std::string_view name, std::span<const std::string> uniforms) {
  const std::string label = quoted("shader '", name, "'");
  if (uniforms.size() > kMaxUniforms) {
    return label + " declares more than " + std::to_string(kMaxUniforms) + " uniforms";
  }
  for (std::size_t i = 0; i < uniforms.size(); ++i) {
    if (std::ranges::find(uniforms.first(i), uniforms[i]) != uniforms.first(i).end()) {
      return label + " declares the uniform '" + uniforms[i] + "' twice";
    }
  }
  GameShader shader{.name = std::string{name}, .uniforms = {uniforms.begin(), uniforms.end()}};
  const std::uint32_t existing = shader_id(name);
  if (existing != 0 && shaders_.at(existing - 1).uniforms == shader.uniforms) {
    return {};
  }

  std::string path{kShaderDirectory};
  path += name;
  path += kShaderExtension;
  const std::optional<std::vector<std::byte>> bytes = vfs_->read(path);
  if (!bytes.has_value()) {
    return label + " not found (no " + path + ")";
  }
  std::string source;
  source.reserve(bytes->size());
  for (const std::byte byte : *bytes) {
    source.push_back(static_cast<char>(byte));
  }
  try {
    shader.compiled = compile_fragment_shader(path, source);
  } catch (const ShaderCompileError& error) {
    return error.what();
  }
  if (shader.compiled.samplers > 1 || shader.compiled.uniform_buffers > 1) {
    return path + ": a shader may use one texture and one constant buffer (see vnmaki.hlsli)";
  }
  if (!shader.uniforms.empty() && shader.compiled.uniform_buffers == 0) {
    return path + ": the shader has uniforms but no constant buffer at register(b0, space3)";
  }
  if (shader.compiled.uniform_buffers == 1) {
    const std::size_t count = std::max<std::size_t>(shader.uniforms.size(), 1);
    shader.uniform_floats = (count + kFloatsPerRow - 1) / kFloatsPerRow * kFloatsPerRow;
  }
  if (existing == 0) {
    shaders_.push_back(std::move(shader));
  } else {
    GameShader& old = shaders_.at(existing - 1);
    if (old.gpu.has_value()) {
      retired_shaders_.push_back(*old.gpu);
    }
    old = std::move(shader);
  }
  return {};
}

std::string Scene::set_shader(const ShaderRequest& request) {
  DisplayProperties* properties = nullptr;
  if (request.layer) {
    const std::optional<Layer> layer = layer_from_name(request.target);
    if (!layer.has_value()) {
      return quoted("unknown layer '", request.target,
                    "' (the layers are background, master, characters, 3d, overlay, ui and "
                    "transient)");
    }
    properties = &layer_state_.at(static_cast<std::size_t>(*layer));
  } else {
    Displayable* shown = find(request.target);
    if (shown == nullptr) {
      return quoted("no displayable '", request.target, "' is shown");
    }
    properties = &shown->properties();
  }

  // The values are worked out first, so a bad name changes nothing.
  std::uint32_t id = properties->shader;
  if (request.shader.has_value()) {
    id = shader_id(*request.shader);
    if (id == 0 && !request.shader->empty()) {
      return quoted("shader '", *request.shader,
                    "' is not declared (use shader(name, { uniforms }) first)");
    }
  }
  const GameShader* shader = shader_by_id(id);
  std::array<float, kMaxUniforms> values{};
  if (id == properties->shader) {
    values = properties->uniforms;
  }
  for (const auto& [uniform, value] : request.uniforms) {
    if (shader == nullptr) {
      return "uniforms need a shader";
    }
    const std::vector<std::string>& names = shader->uniforms;
    const auto found = std::ranges::find(names, uniform);
    if (found == names.end()) {
      return quoted("shader '", shader->name, "' has no uniform '" + uniform + "'");
    }
    if (!(std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max()))) {
      return out_of_range("uniforms." + uniform);
    }
    values.at(static_cast<std::size_t>(found - names.begin())) = static_cast<float>(value);
  }
  properties->shader = id;
  properties->uniforms = values;
  return {};
}

std::vector<std::byte> Scene::capture() const {
  // The properties as a replay would leave them (see the class comment), worked out on copies.
  Timeline settled = timeline_;
  std::map<std::string, DisplayProperties, std::less<>> resting;
  for (const std::string& tag : settled.tags()) {
    if (const DisplayProperties* properties = properties_of(tag)) {
      resting.emplace(tag, *properties);
    }
  }
  settled.settle([&resting](std::string_view tag) -> DisplayProperties* {
    const auto found = resting.find(tag);
    return found == resting.end() ? nullptr : &found->second;
  });

  // ponytail: only images are kept. A displayable of another kind saves itself as a part of its
  // own when it exists.
  const auto image_of = [](const Entry& entry) {
    return dynamic_cast<const ImageDisplayable*>(entry.displayable.get());
  };
  std::uint32_t count = 0;
  for (const std::vector<Entry>& shown : layers_) {
    count += static_cast<std::uint32_t>(std::ranges::count_if(
        shown, [&image_of](const Entry& entry) { return image_of(entry) != nullptr; }));
  }
  Writer displayables;
  displayables.u32(count);
  std::vector<std::pair<std::string_view, const DisplayProperties*>> shaded;
  for (const Layer layer : kLayerOrder) {
    for (const Entry& entry : entries(layer)) {
      if (const ImageDisplayable* image = image_of(entry)) {
        displayables.u8(static_cast<std::uint8_t>(layer));
        displayables.string(entry.tag);
        displayables.string(image->path());
        const auto adjusted = resting.find(entry.tag);
        const DisplayProperties& properties =
            adjusted != resting.end() ? adjusted->second : image->properties();
        displayables.properties(properties);
        if (properties.shader != 0) {
          shaded.emplace_back(entry.tag, &properties);
        }
      }
    }
  }
  std::vector<std::pair<Layer, const DisplayProperties*>> shaded_layers;
  for (const Layer layer : kLayerOrder) {
    const auto adjusted =
        resting.find(std::string{kLayerTagPrefix} + std::string{layer_name(layer)});
    const DisplayProperties& state = adjusted != resting.end()
                                         ? adjusted->second
                                         : layer_state_.at(static_cast<std::size_t>(layer));
    if (state.shader != 0) {
      shaded_layers.emplace_back(layer, &state);
    }
  }

  Writer layered;
  std::uint32_t layered_count = 0;
  for (const std::vector<Entry>& shown : layers_) {
    layered_count +=
        static_cast<std::uint32_t>(std::ranges::count_if(shown, [](const Entry& entry) {
          return dynamic_cast<const LayeredSprite*>(entry.displayable.get()) != nullptr;
        }));
  }
  layered.u32(layered_count);
  for (const Layer layer : kLayerOrder) {
    const std::vector<Entry>& shown = entries(layer);
    for (std::size_t index = 0; index < shown.size(); ++index) {
      const auto* sprite = dynamic_cast<const LayeredSprite*>(shown[index].displayable.get());
      if (sprite == nullptr) {
        continue;
      }
      layered.u8(static_cast<std::uint8_t>(layer));
      layered.u32(static_cast<std::uint32_t>(index));
      layered.string(shown[index].tag);
      layered.string(sprite->name());
      const auto adjusted = resting.find(shown[index].tag);
      const DisplayProperties& properties =
          adjusted != resting.end() ? adjusted->second : sprite->properties();
      layered.properties(properties);
      if (properties.shader != 0) {
        shaded.emplace_back(shown[index].tag, &properties);
      }
      const LayerAttributes attributes = sprite->attributes();
      layered.u32(static_cast<std::uint32_t>(attributes.size()));
      for (const auto& [group, value] : attributes) {
        layered.string(group);
        layered.string(value);
      }
    }
  }

  std::map<std::string, std::vector<std::byte>> parts{
      {std::string{kDisplayablesPart}, displayables.take()},
      {std::string{kLayeredPart}, layered.take()},
      {std::string{kTweensPart}, timeline_.capture()},
  };
  if (!shaded.empty() || !shaded_layers.empty()) {
    Writer shaders;
    shaders.u32(static_cast<std::uint32_t>(shaded.size()));
    for (const auto& [tag, properties] : shaded) {
      shaders.string(tag);
      shaders.string(shader_by_id(properties->shader)->name);
      shaders.uniforms(properties->uniforms);
    }
    shaders.u32(static_cast<std::uint32_t>(shaded_layers.size()));
    for (const auto& [layer, state] : shaded_layers) {
      shaders.u8(static_cast<std::uint8_t>(layer));
      shaders.string(shader_by_id(state->shader)->name);
      shaders.uniforms(state->uniforms);
    }
    parts.emplace(std::string{kShadersPart}, shaders.take());
  }
  Writer writer;
  writer.u32(kFormatVersion);
  writer.u32(static_cast<std::uint32_t>(parts.size()));
  for (const auto& [key, bytes] : parts) {
    writer.string(key);
    writer.block(bytes);
  }
  return writer.take();
}

Scene::Layers Scene::read_displayables(std::span<const std::byte> bytes) {
  Layers layers;
  Reader reader{bytes};
  const std::uint32_t count = reader.u32();
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t layer = reader.u8();
    if (layer >= layers.size()) {
      throw std::invalid_argument{"scene state: unknown layer"};
    }
    std::string tag = reader.string();
    auto displayable = std::make_unique<ImageDisplayable>(reader.string());
    displayable->properties() = reader.properties();
    // push_back without reserve(): `count` is untrusted and may be huge.
    layers.at(layer).push_back(Entry{.tag = std::move(tag), .displayable = std::move(displayable)});
  }
  if (!reader.at_end()) {
    throw std::invalid_argument{"scene state: unexpected data after the last displayable"};
  }
  return layers;
}

void Scene::read_shaders(std::span<const std::byte> bytes, Layers& layers,
                         LayerState& layer_state) const {
  Reader reader{bytes};
  const std::uint32_t count = reader.u32();
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::string tag = reader.string();
    const std::uint32_t id = shader_id(reader.string());
    const std::array<float, kMaxUniforms> values = reader.uniforms();
    if (DisplayProperties* properties = properties_in(layers, layer_state, tag)) {
      properties->shader = id;
      properties->uniforms = values;
    }
  }
  const std::uint32_t layer_count = reader.u32();
  for (std::uint32_t i = 0; i < layer_count; ++i) {
    const std::size_t layer = reader.u8();
    if (layer >= layer_state.size()) {
      throw std::invalid_argument{"scene state: unknown layer"};
    }
    layer_state.at(layer).shader = shader_id(reader.string());
    layer_state.at(layer).uniforms = reader.uniforms();
  }
  if (!reader.at_end()) {
    throw std::invalid_argument{"scene state: unexpected data after the last shader"};
  }
}

void Scene::read_layered(std::span<const std::byte> bytes, Layers& layers) const {
  Reader reader{bytes};
  const std::uint32_t count = reader.u32();
  for (std::uint32_t i = 0; i < count; ++i) {
    const std::size_t layer = reader.u8();
    if (layer >= layers.size()) {
      throw std::invalid_argument{"scene state: unknown layer"};
    }
    const std::uint32_t index = reader.u32();
    std::string tag = reader.string();
    const std::string name = reader.string();
    const auto definition = definitions_.find(name);
    if (definition == definitions_.end()) {
      throw std::invalid_argument{"scene state: unknown layered sprite '" + name + "'"};
    }
    auto sprite = std::make_unique<LayeredSprite>(definition->second);
    sprite->properties() = reader.properties();
    LayerAttributes attributes;
    const std::uint32_t attribute_count = reader.u32();
    for (std::uint32_t j = 0; j < attribute_count; ++j) {
      std::string group = reader.string();
      std::string value = reader.string();
      // push_back without reserve(): `attribute_count` is untrusted and may be huge.
      attributes.emplace_back(std::move(group), std::move(value));
    }
    if (std::string problem = sprite->set_attributes(attributes, false); !problem.empty()) {
      throw std::invalid_argument{"scene state: " + problem};
    }
    // Sprites come in the order of their positions, so each goes back where it was.
    std::vector<Entry>& shown = layers.at(layer);
    const auto position = static_cast<std::ptrdiff_t>(std::min<std::size_t>(index, shown.size()));
    shown.insert(shown.begin() + position,
                 Entry{.tag = std::move(tag), .displayable = std::move(sprite)});
  }
  if (!reader.at_end()) {
    throw std::invalid_argument{"scene state: unexpected data after the last layered sprite"};
  }
}

void Scene::restore(std::span<const std::byte> state) {
  Layers layers;
  LayerState layer_state;
  Timeline tweens;
  // A snapshot taken without a scene has no bytes: nothing was shown.
  if (!state.empty()) {
    Reader reader{state};
    if (reader.u32() != kFormatVersion) {
      throw std::invalid_argument{"scene state: unknown format version"};
    }
    const std::uint32_t part_count = reader.u32();
    std::map<std::string, std::span<const std::byte>, std::less<>> parts;
    for (std::uint32_t i = 0; i < part_count; ++i) {
      std::string key = reader.string();
      const std::span<const std::byte> bytes = reader.block();
      if (!parts.emplace(std::move(key), bytes).second) {
        throw std::invalid_argument{"scene state: a part appears twice"};
      }
    }
    if (!reader.at_end()) {
      throw std::invalid_argument{"scene state: unexpected data after the last part"};
    }
    // A part this version does not know is left out; a part that is missing is empty.
    if (const auto found = parts.find(kDisplayablesPart); found != parts.end()) {
      layers = read_displayables(found->second);
    }
    if (const auto found = parts.find(kLayeredPart); found != parts.end()) {
      read_layered(found->second, layers);
    }
    // After every displayable is back: it finds them by tag.
    if (const auto found = parts.find(kShadersPart); found != parts.end()) {
      read_shaders(found->second, layers, layer_state);
    }
    if (const auto found = parts.find(kTweensPart); found != parts.end()) {
      tweens.restore(found->second, [&layers, &layer_state](std::string_view tag) {
        return properties_in(layers, layer_state, tag);
      });
    }
  }
  layers_ = std::move(layers);
  layer_state_ = std::move(layer_state);
  timeline_ = std::move(tweens);
}

std::vector<std::string> Scene::tags(Layer layer) const {
  const std::vector<Entry>& shown = entries(layer);
  std::vector<std::string> result;
  result.reserve(shown.size());
  for (const Entry& entry : shown) {
    result.push_back(entry.tag);
  }
  return result;
}

std::optional<Layer> Scene::layer_of(std::string_view tag) const {
  const std::optional<Slot> slot = locate(tag);
  if (!slot.has_value()) {
    return std::nullopt;
  }
  return slot->layer;
}

Displayable* Scene::find(std::string_view tag) {
  const std::optional<Slot> slot = locate(tag);
  return slot.has_value() ? entries(slot->layer).at(slot->index).displayable.get() : nullptr;
}

const Displayable* Scene::find(std::string_view tag) const {
  const std::optional<Slot> slot = locate(tag);
  return slot.has_value() ? entries(slot->layer).at(slot->index).displayable.get() : nullptr;
}

void Scene::update(double seconds) {
  timeline_.update(seconds, lookup());
  for (std::vector<Entry>& shown : layers_) {
    for (Entry& entry : shown) {
      entry.displayable->update(seconds);
    }
  }
}

void Scene::prepare(IRenderer& renderer, JobSystem& jobs) {
  const auto texture_of = [&](const std::string& path) -> const Texture& {
    auto found = textures_.find(path);
    if (found == textures_.end()) {
      // ponytail: decodes while the main thread waits and never frees a texture. Preload on
      // jobs and evict by memory budget when a real game hitches or runs out.
      const Image decoded = load_image(*vfs_, jobs, path).get();
      const TextureId texture = renderer.create_texture(decoded, TextureFilter::kLinear);
      found = textures_.emplace(path, Texture{.id = texture, .size = decoded.size}).first;
    }
    return found->second;
  };
  for (std::vector<Entry>& shown : layers_) {
    for (Entry& entry : shown) {
      if (auto* sprite = dynamic_cast<LayeredSprite*>(entry.displayable.get())) {
        for (const std::string& path : sprite->missing_paths()) {
          const Texture& texture = texture_of(path);
          sprite->set_texture(path, texture.id, texture.size);
        }
        continue;
      }
      auto* image = dynamic_cast<ImageDisplayable*>(entry.displayable.get());
      if (image == nullptr || image->has_texture()) {
        continue;
      }
      const Texture& texture = texture_of(image->path());
      image->set_texture(texture.id, texture.size);
    }
  }
  prepare_shaders(renderer);
}

void Scene::prepare_shaders(IRenderer& renderer) {
  for (const ShaderId retired : retired_shaders_) {
    renderer.destroy_shader(retired);
  }
  retired_shaders_.clear();
  // The declared shader of that name with its GPU shader made, or null when it is not declared.
  const auto gpu_shader = [this, &renderer](std::uint32_t id) -> const GameShader* {
    if (shader_by_id(id) == nullptr) {
      return nullptr;
    }
    GameShader& shader = shaders_.at(id - 1);
    if (!shader.gpu.has_value()) {
      shader.gpu = renderer.create_shader(shader.compiled.bytecode());
    }
    return &shader;
  };
  for (std::vector<Entry>& shown : layers_) {
    for (Entry& entry : shown) {
      const GameShader* shader = gpu_shader(entry.displayable->properties().shader);
      entry.displayable->set_shader(shader != nullptr ? *shader->gpu : kSpriteShader,
                                    shader != nullptr ? shader->uniform_floats : 0);
    }
  }
  for (const Layer layer : kLayerOrder) {
    const auto index = static_cast<std::size_t>(layer);
    if (!entries(layer).empty() && gpu_shader(layer_state_.at(index).shader) != nullptr &&
        layer_targets_.at(index) == TextureId{}) {
      layer_targets_.at(index) =
          renderer.create_render_target(virtual_size_, TextureFilter::kNearest);
    }
  }
}

void Scene::draw(IRenderer& renderer, TextureId target) const {
  const RectF whole{.x = 0,
                    .y = 0,
                    .width = static_cast<float>(virtual_size_.width),
                    .height = static_cast<float>(virtual_size_.height)};
  for (const Layer layer : kLayerOrder) {
    const auto index = static_cast<std::size_t>(layer);
    const DisplayProperties& state = layer_state_.at(index);
    const GameShader* shader = shader_by_id(state.shader);
    const TextureId buffer = layer_targets_.at(index);
    // A layer with a shader draws into its own target, which then draws with the shader.
    const bool shaded = shader != nullptr && shader->gpu.has_value() && buffer != TextureId{} &&
                        !entries(layer).empty();
    if (shaded) {
      renderer.begin_pass({.target = buffer, .clear = kTransparent});
    }
    for (const Entry& entry : entries(layer)) {
      entry.displayable->draw(renderer);
    }
    if (shaded) {
      renderer.begin_pass({.target = target});
      renderer.draw(quad_from_rect(whole),
                    DrawState{.texture = buffer,
                              .shader = *shader->gpu,
                              .uniforms = std::as_bytes(
                                  std::span{state.uniforms}.first(shader->uniform_floats))});
    }
  }
}

}  // namespace vnmaki
