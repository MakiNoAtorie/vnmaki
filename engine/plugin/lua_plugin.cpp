#include "engine/plugin/lua_plugin.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <toml++/toml.hpp>

#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kManifestName = "plugin.toml";
constexpr std::string_view kScriptExtension = ".lua";
constexpr int kMaxVersionPart = 1'000'000;

std::string version_text(const Version& version) {
  return std::to_string(version.major) + "." + std::to_string(version.minor) + "." +
         std::to_string(version.patch);
}

// `major[.minor[.patch]]`; nullopt if it is anything else.
std::optional<Version> parse_version(std::string_view text) {
  std::array<int, 3> parts{};
  std::size_t count = 0;
  while (count < parts.size()) {
    const char* const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, parts.at(count));
    if (error != std::errc{} || stop == text.data() || parts.at(count) < 0 ||
        parts.at(count) > kMaxVersionPart) {
      return std::nullopt;
    }
    ++count;
    text.remove_prefix(static_cast<std::size_t>(stop - text.data()));
    if (text.empty()) {
      return Version{.major = parts[0], .minor = parts[1], .patch = parts[2]};
    }
    if (text.front() != '.' || count == parts.size()) {
      return std::nullopt;
    }
    text.remove_prefix(1);
  }
  return std::nullopt;
}

std::string manifest_path(std::string_view name) {
  return lua_plugin_directory(name) + "/" + std::string{kManifestName};
}

[[noreturn]] void fail(std::string_view path, const toml::node* node, std::string_view reason) {
  std::string message{path};
  if (node != nullptr) {
    message += ":" + std::to_string(node->source().begin.line) + ":" +
               std::to_string(node->source().begin.column);
  }
  throw PluginError{message + ": " + std::string{reason}};
}

std::string string_key(const toml::table& table, std::string_view key, std::string_view path,
                       bool required) {
  const toml::node* node = table.get(key);
  if (node == nullptr) {
    if (required) {
      fail(path, nullptr, "missing required key '" + std::string{key} + "'");
    }
    return {};
  }
  const std::optional<std::string> text = node->value_exact<std::string>();
  if (!text || text->empty()) {
    fail(path, node, "'" + std::string{key} + "' must be a non-empty string");
  }
  return *text;
}

void collect_scripts(const Vfs& vfs, const std::string& directory,
                     std::vector<std::string>& paths) {
  for (const VfsEntry& entry : vfs.list(directory)) {
    std::string path = directory + "/" + entry.name;
    if (entry.is_directory) {
      collect_scripts(vfs, path, paths);
    } else if (path.ends_with(kScriptExtension)) {
      paths.push_back(std::move(path));
    }
  }
}

}  // namespace

VersionRange VersionRange::parse(std::string_view text) {
  VersionRange range;
  range.text_ = std::string{text};
  const auto bad = [&](std::string_view why) {
    return PluginError{"engine version range '" + std::string{text} + "': " + std::string{why}};
  };
  std::string_view rest = text;
  const auto skip_separators = [&rest] {
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == ',' || rest.front() == '\t')) {
      rest.remove_prefix(1);
    }
  };
  skip_separators();
  while (!rest.empty()) {
    Bound bound;
    if (rest.starts_with(">=")) {
      bound.op = Op::kGreaterEqual;
      rest.remove_prefix(2);
    } else if (rest.starts_with("<=")) {
      bound.op = Op::kLessEqual;
      rest.remove_prefix(2);
    } else if (rest.starts_with('>')) {
      bound.op = Op::kGreater;
      rest.remove_prefix(1);
    } else if (rest.starts_with('<')) {
      bound.op = Op::kLess;
      rest.remove_prefix(1);
    } else if (rest.starts_with('=')) {
      rest.remove_prefix(1);
    }
    while (rest.starts_with(' ')) {
      rest.remove_prefix(1);
    }
    const std::size_t length = rest.find_first_of(" ,\t");
    const std::optional<Version> version = parse_version(rest.substr(0, length));
    if (!version) {
      throw bad("expected a comparator and a version such as '>=0.1.0', '<0.2'");
    }
    bound.version = *version;
    range.bounds_.push_back(bound);
    rest = length == std::string_view::npos ? std::string_view{} : rest.substr(length);
    skip_separators();
  }
  if (range.bounds_.empty()) {
    throw bad("it is empty");
  }
  return range;
}

bool VersionRange::contains(const Version& version) const {
  const auto key = [](const Version& v) { return std::tie(v.major, v.minor, v.patch); };
  return std::ranges::all_of(bounds_, [&](const Bound& bound) {
    const auto have = key(version);
    const auto want = key(bound.version);
    switch (bound.op) {
      case Op::kGreaterEqual:
        return have >= want;
      case Op::kGreater:
        return have > want;
      case Op::kLessEqual:
        return have <= want;
      case Op::kLess:
        return have < want;
      case Op::kEqual:
        break;
    }
    return have == want;
  });
}

std::string lua_plugin_directory(std::string_view name) { return "plugins/" + std::string{name}; }

LuaPluginManifest parse_plugin_manifest(std::string_view toml_text,
                                        std::string_view directory_name) {
  const std::string path = manifest_path(directory_name);
  toml::table table;
  try {
    table = toml::parse(toml_text, path);
  } catch (const toml::parse_error& error) {
    throw PluginError{path + ":" + std::to_string(error.source().begin.line) + ":" +
                      std::to_string(error.source().begin.column) + ": " +
                      std::string{error.description()}};
  }
  LuaPluginManifest manifest;
  manifest.name = string_key(table, "name", path, false);
  if (!manifest.name.empty() && manifest.name != directory_name) {
    fail(path, table.get("name"),
         "'name' is '" + manifest.name + "' but the folder is '" + std::string{directory_name} +
             "'");
  }
  manifest.name = std::string{directory_name};
  manifest.version = string_key(table, "version", path, false);
  const std::string range = string_key(table, "engine", path, true);
  try {
    manifest.engine = VersionRange::parse(range);
  } catch (const PluginError& error) {
    fail(path, table.get("engine"), error.what());
  }
  return manifest;
}

std::vector<std::string> lua_plugin_scripts(const Vfs& vfs, std::span<const std::string> names,
                                            const Version& engine) {
  std::vector<std::string> paths;
  for (const std::string& name : names) {
    const std::string path = manifest_path(name);
    const std::optional<std::vector<std::byte>> bytes = vfs.read(path);
    if (!bytes) {
      throw PluginError{"plugin '" + name + "' is in project.toml, but " + path + " is missing"};
    }
    std::string text;
    text.reserve(bytes->size());
    for (const std::byte b : *bytes) {
      text.push_back(static_cast<char>(b));
    }
    const LuaPluginManifest manifest = parse_plugin_manifest(text, name);
    if (!manifest.engine.contains(engine)) {
      throw PluginError{"plugin '" + name + "' needs vnmaki " + manifest.engine.text() +
                        ", but this is vnmaki " + version_text(engine) + " (" + path + ")"};
    }
  }
  for (const std::string& name : names) {
    std::vector<std::string> own;
    collect_scripts(vfs, lua_plugin_directory(name), own);
    std::ranges::sort(own);
    paths.insert(paths.end(), std::make_move_iterator(own.begin()),
                 std::make_move_iterator(own.end()));
  }
  return paths;
}

}  // namespace vnmaki
