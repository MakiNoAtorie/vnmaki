#pragma once

// Lua plugins (docs/SPEC.md section 20, docs/plugins.md): `plugins/<name>/plugin.toml` and the
// `.lua` files beside it, read through the Vfs, so they work from a `.vnpak` too. `project.toml`
// lists the active ones (`plugins = ["name"]`). The runtime loads them before `scripts/`.
//
// plugin.toml:
//   name = "flash"                  # optional; if there, it must be the folder name
//   version = "1.0.0"               # optional; for people
//   engine = ">=0.1.0, <0.2.0"      # required: the engine versions the plugin works with

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/core/version.hpp"
#include "engine/plugin/plugin.hpp"

namespace vnmaki {

class Vfs;

/// Engine versions: comparators `>=`, `>`, `<=`, `<`, `=` (or none, which means `=`) and a
/// version `major[.minor[.patch]]` (a missing part is 0), separated by commas or spaces. A
/// version is in the range when it meets every comparator.
class VersionRange {
 public:
  /// Throws PluginError.
  [[nodiscard]] static VersionRange parse(std::string_view text);

  [[nodiscard]] bool contains(const Version& version) const;
  /// The text as it was written.
  [[nodiscard]] const std::string& text() const { return text_; }

 private:
  enum class Op : std::uint8_t { kGreaterEqual, kGreater, kLessEqual, kLess, kEqual };
  struct Bound {
    Op op = Op::kEqual;
    Version version{};
  };

  std::string text_;
  std::vector<Bound> bounds_;
};

struct LuaPluginManifest {
  std::string name{};
  std::string version{};
  VersionRange engine{};
};

/// The folder of a plugin: "plugins/<name>".
[[nodiscard]] std::string lua_plugin_directory(std::string_view name);

/// Reads the text of `plugins/<directory_name>/plugin.toml`. Throws PluginError with a message
/// that names the file and the line.
[[nodiscard]] LuaPluginManifest parse_plugin_manifest(std::string_view toml_text,
                                                      std::string_view directory_name);

/// The `.lua` files of the plugins `names`, in load order: plugin by plugin, and the files of one
/// plugin in the byte order of their paths. It reads each manifest first and throws PluginError
/// if one is missing or bad, or the engine (`engine`) is not in its range, so no script of a
/// plugin that does not fit runs.
[[nodiscard]] std::vector<std::string> lua_plugin_scripts(const Vfs& vfs,
                                                          std::span<const std::string> names,
                                                          const Version& engine);

}  // namespace vnmaki
