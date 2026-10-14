#include "engine/scene/scene.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/render/image.hpp"
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

std::string quoted(std::string_view prefix, std::string_view name, std::string_view suffix) {
  std::string message{prefix};
  message += name;
  message += suffix;
  return message;
}

// --- Serialized form -------------------------------------------------------------------------
// Little-endian. A u32 format version, a u32 count, then for each displayable (layer by layer,
// back to front): u8 layer, tag, image path (each a u32 length and the bytes) and the
// properties as 16 f32: position, anchor, scale, rotation, alpha, tint, crop.

constexpr std::uint32_t kFormatVersion = 1;
constexpr unsigned kBitsPerByte = 8;
constexpr unsigned kBitsPerU32 = 32;
constexpr std::uint32_t kByteMask = 0xFF;

class Writer {
 public:
  void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
  void u32(std::uint32_t value) {
    for (unsigned shift = 0; shift < kBitsPerU32; shift += kBitsPerByte) {
      bytes_.push_back(static_cast<std::byte>((value >> shift) & kByteMask));
    }
  }
  void f32(float value) { u32(std::bit_cast<std::uint32_t>(value)); }
  void string(std::string_view text) {
    u32(static_cast<std::uint32_t>(text.size()));
    for (const char c : text) {
      bytes_.push_back(static_cast<std::byte>(c));
    }
  }
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
  [[nodiscard]] std::vector<std::byte> take() { return std::move(bytes_); }

 private:
  std::vector<std::byte> bytes_;
};

// Throws std::invalid_argument when the bytes end early.
class Reader {
 public:
  explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool at_end() const { return bytes_.empty(); }
  std::uint8_t u8() { return std::to_integer<std::uint8_t>(take(1).front()); }
  std::uint32_t u32() {
    std::uint32_t value = 0;
    unsigned shift = 0;
    for (const std::byte byte : take(sizeof(std::uint32_t))) {
      value |= std::to_integer<std::uint32_t>(byte) << shift;
      shift += kBitsPerByte;
    }
    return value;
  }
  float f32() { return std::bit_cast<float>(u32()); }
  std::string string() {
    const std::span<const std::byte> bytes = take(u32());
    std::string text;
    text.reserve(bytes.size());
    for (const std::byte byte : bytes) {
      text.push_back(std::to_integer<char>(byte));
    }
    return text;
  }
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

 private:
  std::span<const std::byte> take(std::size_t count) {
    if (count > bytes_.size()) {
      throw std::invalid_argument{"scene state: the data ends early"};
    }
    const std::span<const std::byte> taken = bytes_.first(count);
    bytes_ = bytes_.subspan(count);
    return taken;
  }

  std::span<const std::byte> bytes_;
};

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
                       .displayable = ImageDisplayable{std::move(*path)}});
  return {};
}

std::string Scene::show(const ShowRequest& request) {
  std::optional<std::string> path = resolve_image(*vfs_, request.image);
  if (!path.has_value()) {
    return missing_image(request.image);
  }
  const float width = static_cast<float>(virtual_size_.width);
  const float height = static_cast<float>(virtual_size_.height);
  const std::optional<Slot> shown = locate(request.tag);

  Layer layer = Layer::kCharacters;
  DisplayProperties properties;
  properties.position = PointF{.x = kCenter * width, .y = height};
  properties.anchor = kBottomCenter;
  if (shown.has_value()) {
    layer = shown->layer;
    properties = entries(layer).at(shown->index).displayable.properties();
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
    if (!std::isfinite(*request.x)) {
      return "option 'x' must be a finite number";
    }
    properties.position.x = static_cast<float>(*request.x) * width;
  }
  if (request.y.has_value()) {
    if (!std::isfinite(*request.y)) {
      return "option 'y' must be a finite number";
    }
    properties.position.y = static_cast<float>(*request.y) * height;
  }

  ImageDisplayable displayable{std::move(*path)};
  displayable.properties() = properties;
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
}

std::vector<std::byte> Scene::capture() const {
  std::size_t count = 0;
  for (const std::vector<Entry>& shown : layers_) {
    count += shown.size();
  }
  Writer writer;
  writer.u32(kFormatVersion);
  writer.u32(static_cast<std::uint32_t>(count));
  for (const Layer layer : kLayerOrder) {
    for (const Entry& entry : entries(layer)) {
      writer.u8(static_cast<std::uint8_t>(layer));
      writer.string(entry.tag);
      writer.string(entry.displayable.path());
      writer.properties(entry.displayable.properties());
    }
  }
  return writer.take();
}

void Scene::restore(std::span<const std::byte> state) {
  Layers layers;
  // A snapshot taken without a scene has no bytes: nothing was shown.
  if (!state.empty()) {
    Reader reader{state};
    if (reader.u32() != kFormatVersion) {
      throw std::invalid_argument{"scene state: unknown format version"};
    }
    const std::uint32_t count = reader.u32();
    for (std::uint32_t i = 0; i < count; ++i) {
      const std::size_t layer = reader.u8();
      if (layer >= layers.size()) {
        throw std::invalid_argument{"scene state: unknown layer"};
      }
      std::string tag = reader.string();
      ImageDisplayable displayable{reader.string()};
      displayable.properties() = reader.properties();
      // push_back without reserve(): `count` is untrusted and may be huge.
      layers.at(layer).push_back(
          Entry{.tag = std::move(tag), .displayable = std::move(displayable)});
    }
    if (!reader.at_end()) {
      throw std::invalid_argument{"scene state: unexpected data after the last displayable"};
    }
  }
  layers_ = std::move(layers);
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

ImageDisplayable* Scene::find(std::string_view tag) {
  const std::optional<Slot> slot = locate(tag);
  return slot.has_value() ? &entries(slot->layer).at(slot->index).displayable : nullptr;
}

const ImageDisplayable* Scene::find(std::string_view tag) const {
  const std::optional<Slot> slot = locate(tag);
  return slot.has_value() ? &entries(slot->layer).at(slot->index).displayable : nullptr;
}

void Scene::update(double seconds) {
  for (std::vector<Entry>& shown : layers_) {
    for (Entry& entry : shown) {
      entry.displayable.update(seconds);
    }
  }
}

void Scene::prepare(IRenderer& renderer, JobSystem& jobs) {
  for (std::vector<Entry>& shown : layers_) {
    for (Entry& entry : shown) {
      if (entry.displayable.has_texture()) {
        continue;
      }
      const std::string& path = entry.displayable.path();
      auto found = textures_.find(path);
      if (found == textures_.end()) {
        // ponytail: decodes while the main thread waits and never frees a texture. Preload on
        // jobs and evict by memory budget when a real game hitches or runs out.
        const Image image = load_image(*vfs_, jobs, path).get();
        const TextureId texture = renderer.create_texture(image, TextureFilter::kLinear);
        found = textures_.emplace(path, Texture{.id = texture, .size = image.size}).first;
      }
      entry.displayable.set_texture(found->second.id, found->second.size);
    }
  }
}

void Scene::draw(IRenderer& renderer) const {
  for (const Layer layer : kLayerOrder) {
    for (const Entry& entry : entries(layer)) {
      entry.displayable.draw(renderer);
    }
  }
}

}  // namespace vnmaki
