#pragma once

// The C++ plugin API (docs/SPEC.md section 20, docs/plugins.md). A plugin is a static library
// with one function, `void register_plugin_<name>(PluginContext&)` in namespace `vnmaki`, that
// registers what the plugin adds. This header has no Lua and no sol2: values cross as
// PluginValue and the engine converts them.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/plugin/plugin_value.hpp"
#include "engine/render/image.hpp"
#include "engine/render/renderer.hpp"
#include "engine/scene/displayable.hpp"

namespace vnmaki {

/// A Lua function of a plugin. It gets the arguments of the call. It returns the result (nil
/// for none) or throws PluginError, which is a Lua error at the line of the call. A script may
/// run again in a replay (a load, a rollback), so a function must give the same result for the
/// same arguments and must not change what the game state depends on.
using PluginFunction = std::function<PluginValue(std::span<const PluginValue>)>;

/// A displayable that a plugin adds. The scene calls `update` once per fixed step and `draw` back
/// to front like any other displayable. It is made again from its type and params when a
/// snapshot is restored, so what it keeps besides the `properties()` is not saved.
class PluginDisplayable : public Displayable {
 public:
  [[nodiscard]] const std::string& type() const { return type_; }
  [[nodiscard]] const PluginParams& params() const { return params_; }
  /// For the scene: names the type and params that made it.
  void set_origin(std::string type, PluginParams params) {
    type_ = std::move(type);
    params_ = std::move(params);
  }
  /// For the scene: a 1 by 1 white texture, to draw solid colours (a quad's `tint` is the
  /// colour). It is the zero id until the scene's first prepare().
  void set_white_texture(TextureId texture) { white_ = texture; }

 protected:
  [[nodiscard]] TextureId white_texture() const { return white_; }

 private:
  std::string type_;
  PluginParams params_;
  TextureId white_{};
};

/// Makes a displayable of one type from the params of `show`. Throws PluginError for params that
/// do not fit (the error reaches the script at the line of the `show`).
using PluginDisplayableFactory =
    std::function<std::unique_ptr<PluginDisplayable>(const PluginParams&)>;

/// An image of a file format, with straight (not premultiplied) alpha: `rgba` has 4 bytes for
/// each pixel, rows from top to bottom.
struct PluginImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> rgba{};
};

/// Decodes a whole file. Throws PluginError if it is bad. It runs on a job thread, so it must not
/// touch shared state without a lock.
using PluginImageLoader = std::function<PluginImage(std::span<const std::byte>)>;

/// What a plugin adds to the engine. The engine reads the lists after the plugins registered.
/// Names are unique across all plugins; a plugin that registers a taken name gets PluginError.
class PluginContext {
 public:
  /// The context that plugins linked into the runner (`-DVNMAKI_PLUGINS=...`, see
  /// cmake/VnmakiPlugins.cmake) register in before `main`. Runtimes and scenes use it unless they
  /// are given another.
  [[nodiscard]] static PluginContext& linked();

  /// A Lua function: a global name for scripts. The name is a Lua identifier and is not used.
  void add_function(std::string name, PluginFunction function);
  /// A displayable type, for `show(tag, { displayable = type, params = { ... } })`.
  void add_displayable(std::string type, PluginDisplayableFactory factory);
  /// A file format of images: `images/<name><extension>` is found by `show` after the built-in
  /// ones. `extension` is lower case with the dot (".ppm"). The table of formats is process-wide
  /// (engine/render/image.hpp), not part of this context.
  void add_image_loader(std::string extension, PluginImageLoader loader);

  [[nodiscard]] const std::map<std::string, PluginFunction, std::less<>>& functions() const {
    return functions_;
  }
  /// The factory of `type`, or null.
  [[nodiscard]] const PluginDisplayableFactory* find_displayable(std::string_view type) const;
  /// The types, sorted.
  [[nodiscard]] std::vector<std::string> displayable_types() const;

 private:
  std::map<std::string, PluginFunction, std::less<>> functions_;
  std::map<std::string, PluginDisplayableFactory, std::less<>> displayables_;
};

}  // namespace vnmaki
