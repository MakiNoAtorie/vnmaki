#include "engine/scene/layered_sprite.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vnmaki {

namespace {

// A group of one fixed image has a value without a name.
bool is_fixed(const LayeredDefinition::Group& group) {
  return group.values.size() == 1 && group.values.front().name.empty();
}

std::string unknown_group(const LayeredDefinition& definition, std::string_view group) {
  std::string message = "unknown group '";
  message += group;
  message += "' for '" + definition.name + "' (the groups are ";
  for (const LayeredDefinition::Group& known : definition.groups) {
    message += known.name;
    message += &known == &definition.groups.back() ? ")" : ", ";
  }
  return message;
}

std::string unknown_value(const LayeredDefinition& definition,
                          const LayeredDefinition::Group& group, std::string_view value) {
  std::string message = "unknown value '";
  message += value;
  message += "' for group '" + group.name + "' of '" + definition.name + "' (";
  if (is_fixed(group)) {
    return message + "it has one fixed image)";
  }
  message += "the values are ";
  for (const LayeredDefinition::Value& known : group.values) {
    message += known.name;
    message += &known == &group.values.back() ? ")" : ", ";
  }
  return message;
}

}  // namespace

LayeredSprite::LayeredSprite(std::shared_ptr<const LayeredDefinition> definition)
    : definition_(std::move(definition)), layers_(definition_->groups.size()) {
  for (std::size_t group = 0; group < layers_.size(); ++group) {
    if (is_fixed(definition_->groups[group])) {
      layers_[group].current = 0;
    }
  }
  // The definition was checked when it was made, so the defaults are known groups and values.
  [[maybe_unused]] const std::string problem = set_attributes(definition_->defaults, false);
}

LayerAttributes LayeredSprite::attributes() const {
  LayerAttributes result;
  result.reserve(layers_.size());
  for (std::size_t group = 0; group < layers_.size(); ++group) {
    if (layers_[group].current.has_value()) {
      result.emplace_back(definition_->groups[group].name,
                          definition_->groups[group].values.at(*layers_[group].current).name);
    }
  }
  return result;
}

std::string LayeredSprite::set_attributes(const LayerAttributes& changes, bool crossfade) {
  std::vector<std::pair<std::size_t, std::size_t>> resolved;  // (group, value) indices
  resolved.reserve(changes.size());
  for (const auto& [group_name, value_name] : changes) {
    const auto group = std::ranges::find(definition_->groups, group_name,
                                         &LayeredDefinition::Group::name);
    if (group == definition_->groups.end()) {
      return unknown_group(*definition_, group_name);
    }
    const auto value =
        std::ranges::find(group->values, value_name, &LayeredDefinition::Value::name);
    if (value == group->values.end()) {
      return unknown_value(*definition_, *group, value_name);
    }
    resolved.emplace_back(static_cast<std::size_t>(group - definition_->groups.begin()),
                          static_cast<std::size_t>(value - group->values.begin()));
  }

  const bool fades = crossfade && definition_->crossfade > 0;
  for (const auto& [group, value] : resolved) {
    Layer& layer = layers_[group];
    if (layer.current == value) {
      continue;
    }
    layer.previous = layer.current;
    layer.previous_play = layer.play;
    layer.current = value;
    layer.play = 0;
    layer.fading = fades;
    layer.elapsed = 0;
    if (!fades) {
      layer.previous.reset();
    }
  }
  if (!crossfade) {
    // A cut ends every fade, also of groups the change did not name.
    for (Layer& layer : layers_) {
      layer.fading = false;
      layer.previous.reset();
    }
  }
  return {};
}

std::optional<float> LayeredSprite::fade_progress(std::string_view group) const {
  for (std::size_t i = 0; i < layers_.size(); ++i) {
    if (definition_->groups[i].name == group && layers_[i].fading) {
      return std::min(1.0F, static_cast<float>(layers_[i].elapsed / definition_->crossfade));
    }
  }
  return std::nullopt;
}

LayeredSprite::Picture LayeredSprite::picture_of(std::size_t group, std::size_t value,
                                                 double play) const {
  const LayeredDefinition::Value& chosen = definition_->groups.at(group).values.at(value);
  if (chosen.animation == nullptr) {
    return Picture{.path = &chosen.path};
  }
  const AnimationDefinition::Frame& frame =
      chosen.animation->frames.at(chosen.animation->frame_at(play));
  return Picture{.path = &frame.path, .uv = frame.uv, .animation = chosen.animation.get()};
}

Size LayeredSprite::picture_size(const Picture& picture) const {
  const Texture* texture = texture_of(*picture.path);
  if (texture == nullptr) {
    return {};
  }
  return picture.animation != nullptr ? picture.animation->frame_size(texture->size)
                                      : texture->size;
}

bool LayeredSprite::animating() const {
  for (std::size_t group = 0; group < layers_.size(); ++group) {
    if (!layers_[group].current.has_value()) {
      continue;
    }
    const auto& animation = definition_->groups[group].values.at(*layers_[group].current).animation;
    if (animation != nullptr && animation->running_at(layers_[group].play)) {
      return true;
    }
  }
  return false;
}

void LayeredSprite::finish_animations() {
  for (std::size_t group = 0; group < layers_.size(); ++group) {
    if (!layers_[group].current.has_value()) {
      continue;
    }
    const auto& animation = definition_->groups[group].values.at(*layers_[group].current).animation;
    if (animation != nullptr && !animation->loop) {
      layers_[group].play = std::max(layers_[group].play, animation->duration());
    }
  }
}

const LayeredSprite::Texture* LayeredSprite::texture_of(const std::string& path) const {
  const auto found = textures_.find(path);
  return found == textures_.end() ? nullptr : &found->second;
}

std::vector<std::string> LayeredSprite::missing_paths() const {
  std::vector<std::string> missing;
  for (std::size_t group = 0; group < layers_.size(); ++group) {
    const Layer& layer = layers_[group];
    for (const std::optional<std::size_t>& value : {layer.current, layer.previous}) {
      if (!value.has_value()) {
        continue;
      }
      const LayeredDefinition::Value& chosen = definition_->groups.at(group).values.at(*value);
      const auto need = [&](const std::string& path) {
        if (texture_of(path) == nullptr && std::ranges::find(missing, path) == missing.end()) {
          missing.push_back(path);
        }
      };
      // An animation needs all its frames at once, so none loads while it plays.
      if (chosen.animation == nullptr) {
        need(chosen.path);
        continue;
      }
      for (const AnimationDefinition::Frame& frame : chosen.animation->frames) {
        need(frame.path);
      }
    }
  }
  return missing;
}

void LayeredSprite::set_texture(const std::string& path, TextureId texture, Size size) {
  textures_[path] = Texture{.id = texture, .size = size};
}

void LayeredSprite::update(double seconds) {
  for (Layer& layer : layers_) {
    layer.play += seconds;
    if (!layer.fading) {
      continue;
    }
    layer.elapsed += seconds;
    if (layer.elapsed >= definition_->crossfade) {
      layer.fading = false;
      layer.previous.reset();
    }
  }
}

Size LayeredSprite::base_size() const {
  if (layers_.empty() || !layers_.front().current.has_value()) {
    return {};
  }
  return picture_size(picture_of(0, *layers_.front().current, layers_.front().play));
}

void LayeredSprite::draw_layer(IRenderer& renderer, const Picture& picture, Size base,
                               float opacity) const {
  const Texture* texture = texture_of(*picture.path);
  if (texture == nullptr) {
    return;
  }
  const Size size = picture_size(picture);
  // The window of the crop and the layer, both in pixels from the sprite's origin.
  const RectF& crop = properties().crop;
  const auto base_width = static_cast<float>(base.width);
  const auto base_height = static_cast<float>(base.height);
  const auto layer_width = static_cast<float>(size.width);
  const auto layer_height = static_cast<float>(size.height);
  const float window_left = crop.x * base_width;
  const float window_top = crop.y * base_height;
  const float window_width = crop.width * base_width;
  const float window_height = crop.height * base_height;
  const float left = std::max(window_left, 0.0F);
  const float top = std::max(window_top, 0.0F);
  const float right = std::min(window_left + window_width, layer_width);
  const float bottom = std::min(window_top + window_height, layer_height);
  if (!(right > left && bottom > top)) {
    return;
  }
  const RectF part{.x = (left - window_left) / window_width,
                   .y = (top - window_top) / window_height,
                   .width = (right - left) / window_width,
                   .height = (bottom - top) / window_height};
  const RectF uv = uv_within(picture.uv, RectF{.x = left / layer_width,
                                               .y = top / layer_height,
                                               .width = (right - left) / layer_width,
                                               .height = (bottom - top) / layer_height});
  Color tint = properties().tint;
  tint.a *= properties().alpha * opacity;
  renderer.draw(Quad{.corners = place_corners(properties(), base, part), .uv = uv, .tint = tint},
                draw_state(texture->id));
}

void LayeredSprite::draw(IRenderer& renderer) const {
  const Size base = base_size();
  if (base.width <= 0 || base.height <= 0) {
    return;
  }
  for (std::size_t group = 0; group < layers_.size(); ++group) {
    const Layer& layer = layers_[group];
    float opacity = 1;
    if (layer.fading) {
      // The new image fades in over the old one, which stays whole: no see-through dip.
      if (layer.previous.has_value()) {
        draw_layer(renderer, picture_of(group, *layer.previous, layer.previous_play), base, 1);
      }
      opacity = std::min(1.0F, static_cast<float>(layer.elapsed / definition_->crossfade));
    }
    if (layer.current.has_value()) {
      draw_layer(renderer, picture_of(group, *layer.current, layer.play), base, opacity);
    }
  }
}

RectF LayeredSprite::bounds() const {
  return bounding_box(place_corners(properties(), base_size()));
}

}  // namespace vnmaki
