#include "engine/assets/importer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include "engine/assets/audio_import.hpp"
#include "engine/assets/font_import.hpp"
#include "engine/assets/gltf_import.hpp"
#include "engine/render/precompiled_shader.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/render/shader_headers.hpp"
#include "engine/tools/story_graph.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kShaderImporter = "shader";
constexpr std::uint32_t kShaderImporterVersion = 1;
constexpr std::string_view kShaderDirectory = "shaders/";
constexpr std::string_view kHlslSuffix = ".frag.hlsl";
constexpr std::string_view kCacheRoot = ".vnmaki";
// The engine headers that a game shader includes: a change of one is a change of every shader.
constexpr std::array<std::string_view, 4> kShaderHeaders{"vnmaki.hlsli", "sprite_fragment.hlsli",
                                                         "mesh.hlsli", "transition.hlsli"};

enum class Kind : std::uint8_t { kNone, kAudio, kFont, kShader, kGltf, kText };

std::string extension_of(std::string_view path) {
  const std::size_t dot = path.rfind('.');
  const std::size_t slash = path.rfind('/');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
    return {};
  }
  std::string extension{path.substr(dot)};
  std::ranges::transform(extension, extension.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return extension;
}

std::string_view without_extension(std::string_view path) {
  return path.substr(0, path.size() - extension_of(path).size());
}

Kind kind_of(std::string_view path) {
  const std::string extension = extension_of(path);
  if (extension == ".wav" || extension == ".flac") {
    return Kind::kAudio;
  }
  if (extension == ".ttf" || extension == ".otf") {
    return Kind::kFont;
  }
  if (extension == ".gltf" || extension == ".glb") {
    return Kind::kGltf;
  }
  if (path.starts_with(kShaderDirectory) && path.ends_with(kHlslSuffix)) {
    return Kind::kShader;
  }
  // Changes that can change a font subset: the scripts, the translations and project.toml.
  if (extension == ".lua" || extension == ".po" || path == kProjectConfigPath) {
    return Kind::kText;
  }
  return Kind::kNone;
}

bool is_language_code(std::string_view language) {
  return !language.empty() && std::ranges::all_of(language, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_';
  });
}

std::vector<std::byte> bytes_of(std::string_view text) {
  std::vector<std::byte> bytes;
  bytes.reserve(text.size());
  for (const char c : text) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
  std::string text;
  text.reserve(bytes.size());
  for (const std::byte byte : bytes) {
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

// Every file under `directory`, as virtual paths in order. The folder `.vnmaki` has the cache.
void list_files(const Vfs& vfs, const std::string& directory, std::vector<std::string>& out) {
  for (const VfsEntry& entry : vfs.list(directory)) {
    const std::string path = directory.empty() ? entry.name : directory + "/" + entry.name;
    if (!entry.is_directory) {
      out.push_back(path);
    } else if (path != kCacheRoot) {
      list_files(vfs, path, out);
    }
  }
}

// The output of one importer: from the cache, or made and stored.
ImportedFile cached_or_made(const ImportCache& cache, const std::string& source,
                            const std::vector<std::byte>& input, std::string_view importer,
                            std::uint32_t version, std::string_view params, std::string path,
                            const std::function<std::vector<std::byte>()>& make) {
  const std::string key = import_cache_key(input, importer, version, params);
  ImportedFile file{.source = source, .path = std::move(path)};
  if (std::optional<std::vector<std::byte>> hit = cache.find(key)) {
    file.data = std::move(*hit);
    file.cached = true;
    return file;
  }
  file.data = make();
  cache.store(key, file.data);
  return file;
}

}  // namespace

Importer::Importer(const Vfs& vfs, std::filesystem::path project_dir)
    : vfs_(&vfs), cache_(ImportCache::for_project(project_dir)) {}

ImportReport Importer::import_all() const {
  std::vector<std::string> sources;
  list_files(*vfs_, {}, sources);
  return import_paths(sources);
}

ImportReport Importer::import_changed(std::span<const std::string> paths) const {
  std::set<std::string> sources;
  bool fonts = false;
  for (const std::string& path : paths) {
    if (path.starts_with(kCacheRoot) || !vfs_->exists(path)) {
      continue;
    }
    const Kind kind = kind_of(path);
    fonts = fonts || kind == Kind::kText;
    if (kind != Kind::kNone && kind != Kind::kText) {
      sources.insert(path);
    }
  }
  if (fonts) {
    std::vector<std::string> all;
    list_files(*vfs_, {}, all);
    std::ranges::copy_if(all, std::inserter(sources, sources.end()),
                         [](const std::string& path) { return kind_of(path) == Kind::kFont; });
  }
  return import_paths({sources.begin(), sources.end()});
}

ImportReport Importer::import_paths(const std::vector<std::string>& sources) const {
  const std::optional<std::vector<std::byte>> toml = vfs_->read(kProjectConfigPath);
  const ProjectConfig config = load_project_config(*vfs_);
  const ImportSettings settings = parse_import_settings(toml ? text_of(*toml) : std::string{});
  const std::string audio_params = std::to_string(static_cast<int>(settings.normalize)) + "," +
                                   std::to_string(settings.target_db);
  std::string header_text;
  for (const std::string_view name : kShaderHeaders) {
    header_text += hlsl_header(name);
  }
  const std::string shader_params = std::to_string(hash_bytes(bytes_of(header_text)));

  std::vector<ScriptSource> scripts;  // for the fonts, read when the first font needs them
  std::map<std::string, std::set<char32_t>> codepoints;

  ImportReport report;
  for (const std::string& source : sources) {
    const Kind kind = kind_of(source);
    try {
      const std::optional<std::vector<std::byte>> input = vfs_->read(source);
      if (!input) {
        throw std::runtime_error{"cannot be read"};
      }
      switch (kind) {
        case Kind::kAudio:
          report.files.push_back(
              cached_or_made(cache_, source, *input, kAudioImporter, kAudioImporterVersion,
                             audio_params, std::string{without_extension(source)} + ".opus",
                             [&] { return import_audio(*input, settings).opus; }));
          break;
        case Kind::kShader:
          report.files.push_back(cached_or_made(
              cache_, source, *input, kShaderImporter, kShaderImporterVersion, shader_params,
              std::string{without_extension(without_extension(source))} + ".frag" +
                  std::string{kPrecompiledShaderExtension},
              [&] {
                if (!shader_compiler_available()) {
                  throw std::runtime_error{
                      "cannot be compiled: this build has no shader compiler (SDL_shadercross)"};
                }
                return write_precompiled_shader(compile_fragment_shader(source, text_of(*input)));
              }));
          break;
        case Kind::kFont:
          if (scripts.empty()) {
            scripts = read_scripts(*vfs_);
          }
          for (const std::string& language : config.languages) {
            if (!is_language_code(language)) {
              throw std::runtime_error{"project.toml: '" + language + "' is not a language code"};
            }
            auto found = codepoints.find(language);
            if (found == codepoints.end()) {
              found = codepoints
                          .emplace(language, language_codepoints(*vfs_, config, scripts, language))
                          .first;
            }
            const std::set<char32_t>& wanted = found->second;
            report.files.push_back(cached_or_made(
                cache_, source, *input, kFontImporter, kFontImporterVersion,
                language + ":" + codepoints_text(wanted),
                std::string{without_extension(source)} + "." + language + extension_of(source),
                [&] { return subset_font(*input, wanted).font; }));
          }
          break;
        case Kind::kGltf: {
          const std::vector<std::string> found = validate_gltf(*vfs_, source);
          report.problems.insert(report.problems.end(), found.begin(), found.end());
          if (found.empty()) {
            report.files.push_back({.source = source});
          }
          break;
        }
        case Kind::kNone:
        case Kind::kText:
          break;
      }
    } catch (const std::exception& error) {
      const std::string_view what = error.what();
      // A message that starts with the file ("translations/ja.po:5: ...") already names its file.
      report.problems.push_back(what.starts_with("translations/") ||
                                        what.starts_with("project.toml")
                                    ? std::string{what}
                                    : source + ": " + std::string{what});
    }
  }
  return report;
}

void write_imported_files(const ImportReport& report, const std::filesystem::path& directory) {
  for (const ImportedFile& file : report.files) {
    if (file.path.empty()) {
      continue;
    }
    const std::filesystem::path target = directory / std::filesystem::path{file.path};
    std::error_code ignored;
    std::filesystem::create_directories(target.parent_path(), ignored);
    std::ofstream out{target, std::ios::binary | std::ios::trunc};
    out.write(reinterpret_cast<const char*>(file.data.data()),
              static_cast<std::streamsize>(file.data.size()));
    if (!out.flush()) {
      throw std::runtime_error{"cannot write " + target.string()};
    }
  }
}

}  // namespace vnmaki
