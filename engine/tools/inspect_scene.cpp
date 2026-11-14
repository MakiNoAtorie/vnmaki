#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/anim/timeline.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/scene/frame_animation.hpp"
#include "engine/scene/layered_sprite.hpp"
#include "engine/scene/movie_displayable.hpp"
#include "engine/tools/inspect.hpp"

namespace vnmaki {

namespace {

// Edits stay inside this range, as a script's positions do (kMaxPositionFraction of a canvas).
constexpr double kMaxEdit = 1'000'000.0;

// A property by name and where it lives; `unit` ones are colour and opacity, kept in [0, 1].
struct Field {
  std::string name;
  float* value = nullptr;
  bool unit = false;
};

std::vector<Field> fields_of(DisplayProperties& p) {
  std::vector<Field> fields{
      {"x", &p.position.x},           {"y", &p.position.y},        {"anchor_x", &p.anchor.x},
      {"anchor_y", &p.anchor.y},      {"scale_x", &p.scale.x},     {"scale_y", &p.scale.y},
      {"rotation", &p.rotation},      {"alpha", &p.alpha, true},   {"tint_r", &p.tint.r, true},
      {"tint_g", &p.tint.g, true},    {"tint_b", &p.tint.b, true}, {"tint_a", &p.tint.a, true},
      {"crop_x", &p.crop.x},          {"crop_y", &p.crop.y},       {"crop_width", &p.crop.width},
      {"crop_height", &p.crop.height}};
  if (p.shader != 0) {
    for (std::size_t i = 0; i < p.uniforms.size(); ++i) {
      fields.push_back({"uniform_" + std::to_string(i), &p.uniforms.at(i)});
    }
  }
  return fields;
}

std::string kind_of(const Displayable& displayable) {
  if (const auto* image = dynamic_cast<const ImageDisplayable*>(&displayable)) {
    return "image " + image->path();
  }
  if (dynamic_cast<const LayeredSprite*>(&displayable) != nullptr) {
    return "layered";
  }
  if (dynamic_cast<const FrameAnimation*>(&displayable) != nullptr) {
    return "animation";
  }
  if (dynamic_cast<const MovieDisplayable*>(&displayable) != nullptr) {
    return "movie";
  }
  return "displayable";
}

// The name of a Timeline slot: a property or a shader uniform.
std::string channel_name(std::size_t channel) {
  DisplayProperties properties;
  properties.shader = 1;  // so that the uniforms have names
  const std::vector<Field> fields = fields_of(properties);
  // fields_of lists the channels in the order of Channel, then the uniforms.
  return channel < fields.size() ? fields.at(channel).name : "?";
}

}  // namespace

std::vector<LayerRows> inspect_scene(const Scene& scene) {
  std::vector<LayerRows> layers;
  layers.reserve(kLayerOrder.size());
  for (const Layer layer : kLayerOrder) {
    LayerRows rows{.layer = std::string{layer_name(layer)}};
    for (const std::string& tag : scene.tags(layer)) {
      const Displayable* displayable = scene.find(tag);
      rows.displayables.push_back(
          {.tag = tag, .kind = displayable != nullptr ? kind_of(*displayable) : "displayable"});
    }
    layers.push_back(std::move(rows));
  }
  return layers;
}

std::vector<PropertyRow> inspect_properties(const Scene& scene, std::string_view tag) {
  const Displayable* displayable = scene.find(tag);
  if (displayable == nullptr) {
    return {};
  }
  DisplayProperties copy = displayable->properties();
  std::vector<PropertyRow> rows;
  for (const Field& field : fields_of(copy)) {
    rows.push_back({.name = field.name, .value = static_cast<double>(*field.value)});
  }
  return rows;
}

std::string set_property(Scene& scene, std::string_view tag, std::string_view name, double value) {
  Displayable* displayable = scene.find(tag);
  if (displayable == nullptr) {
    return "no displayable '" + std::string{tag} + "' is shown";
  }
  if (!std::isfinite(value)) {
    return "the value is not a number";
  }
  for (const Field& field : fields_of(displayable->properties())) {
    if (field.name == name) {
      *field.value = static_cast<float>(field.unit ? std::clamp(value, 0.0, 1.0)
                                                   : std::clamp(value, -kMaxEdit, kMaxEdit));
      return {};
    }
  }
  return "no property '" + std::string{name} + "'";
}

std::vector<TweenRow> inspect_tweens(const Scene& scene) {
  std::vector<TweenRow> rows;
  for (const TweenInfo& info : scene.timeline().active()) {
    TweenRow row{.tag = info.tag,
                 .ease = std::string{ease_name(info.ease)},
                 .elapsed = info.elapsed,
                 .duration = info.duration,
                 .loop = info.loop};
    for (const std::size_t channel : info.channels) {
      row.channels += (row.channels.empty() ? "" : ", ") + channel_name(channel);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace vnmaki
