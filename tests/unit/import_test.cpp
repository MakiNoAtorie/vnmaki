// Asset import (engine/assets/): the cache, the audio gain, font subsets, glTF checks, the project
// importer and the precompiled shader file. The inputs come from
// tools/generators/import_fixtures.py.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <doctest/doctest.h>

#include "engine/assets/audio_import.hpp"
#include "engine/assets/font_import.hpp"
#include "engine/assets/gltf_import.hpp"
#include "engine/assets/import_cache.hpp"
#include "engine/assets/import_settings.hpp"
#include "engine/assets/importer.hpp"
#include "engine/audio/audio_decoder.hpp"
#include "engine/render/precompiled_shader.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/scene/scene.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"
#include "tests/support/temp_dir.hpp"

namespace {

namespace fs = std::filesystem;
using vnmaki::AudioNormalize;
using vnmaki::ImportSettings;

const fs::path& fixtures() {
  static const fs::path path = fs::path{VNMAKI_FIXTURES_DIR} / "import";
  return path;
}

std::vector<std::byte> read_bytes(const fs::path& path) {
  std::ifstream in{path, std::ios::binary};
  REQUIRE(in);
  const std::vector<char> chars{std::istreambuf_iterator<char>{in},
                                std::istreambuf_iterator<char>{}};
  std::vector<std::byte> bytes;
  for (const char c : chars) {
    bytes.push_back(static_cast<std::byte>(c));
  }
  return bytes;
}

void write_bytes(const fs::path& path, std::span<const std::byte> bytes) {
  std::ofstream out{path, std::ios::binary | std::ios::trunc};
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// A folder in the temp directory that goes when the test ends.
class TempDir {
 public:
  explicit TempDir(std::string_view name)
      : path_(vnmaki::test::unique_temp_path("vnmaki_import_", name)) {
    fs::remove_all(path_);
    fs::create_directories(path_);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
  ~TempDir() {
    std::error_code ignored;
    fs::remove_all(path_, ignored);
  }
  [[nodiscard]] const fs::path& path() const { return path_; }

 private:
  fs::path path_;
};

// The peak and the RMS level of all samples of an encoded file.
struct Levels {
  double peak = 0;
  double rms = 0;
  std::uint64_t frames = 0;
};

Levels levels_of(std::vector<std::byte> encoded) {
  vnmaki::AudioDecoder decoder{std::move(encoded)};
  std::array<float, 2048> block{};
  Levels levels;
  double energy = 0;
  std::uint64_t samples = 0;
  for (std::size_t frames = decoder.read(block); frames > 0; frames = decoder.read(block)) {
    for (const float sample : std::span{block}.first(frames * vnmaki::kAudioChannels)) {
      levels.peak = std::max(levels.peak, static_cast<double>(std::abs(sample)));
      energy += static_cast<double>(sample) * static_cast<double>(sample);
    }
    samples += frames * vnmaki::kAudioChannels;
    levels.frames += frames;
  }
  levels.rms = std::sqrt(energy / static_cast<double>(samples));
  return levels;
}

double db(double level) { return 20 * std::log10(level); }

constexpr double kTone = 0.25;   // the amplitude of tone.wav
constexpr double kQuiet = 0.05;  // the amplitude of quiet.wav
constexpr double kGainTolerance = 0.01;
constexpr double kOpusTolerance = 0.06;  // Opus is lossy
constexpr double kHalfSecondFrames = 24000;
// The box font of the fixtures has a glyph for 68 of the 95 printable ASCII characters.
constexpr std::uint32_t kAsciiGlyphs = 68;
constexpr double kSqrtTwo = 1.41421356237;
constexpr double kPeakTargetDb = -3;
constexpr double kRmsTargetDb = -18;

}  // namespace

TEST_CASE(
    "import cache: a key changes with the source, the importer, its version and its settings") {
  const std::vector<std::byte> source{std::byte{1}, std::byte{2}, std::byte{3}};
  const std::vector<std::byte> other{std::byte{1}, std::byte{2}, std::byte{4}};
  const std::string key = vnmaki::import_cache_key(source, "audio", 1, "a");
  CHECK(key == vnmaki::import_cache_key(source, "audio", 1, "a"));
  CHECK(key.starts_with("audio-"));
  CHECK(key != vnmaki::import_cache_key(other, "audio", 1, "a"));
  CHECK(key != vnmaki::import_cache_key(source, "font", 1, "a"));
  CHECK(key != vnmaki::import_cache_key(source, "audio", 2, "a"));
  CHECK(key != vnmaki::import_cache_key(source, "audio", 1, "b"));
}

TEST_CASE("import cache: a stored entry is a hit, anything else is a miss") {
  const TempDir temp{"cache"};
  const vnmaki::ImportCache cache = vnmaki::ImportCache::for_project(temp.path());
  const std::vector<std::byte> source{std::byte{7}};
  const std::vector<std::byte> output{std::byte{9}, std::byte{0}, std::byte{8}};
  const std::string key = vnmaki::import_cache_key(source, "audio", 1);

  CHECK_FALSE(cache.find(key).has_value());
  cache.store(key, output);
  REQUIRE(cache.find(key).has_value());
  CHECK(*cache.find(key) == output);
  CHECK(fs::exists(temp.path() / ".vnmaki" / "cache" / key));
  CHECK_FALSE(cache.find(vnmaki::import_cache_key(source, "audio", 2)).has_value());  // a miss
}

TEST_CASE("audio import: the gain of each mode") {
  const ImportSettings none{};
  const ImportSettings peak{.normalize = AudioNormalize::kPeak, .target_db = -6};
  const ImportSettings rms{.normalize = AudioNormalize::kRms, .target_db = -20};
  const double minus_six = std::pow(10.0, -6.0 / 20.0);
  const double minus_twenty = std::pow(10.0, -20.0 / 20.0);
  CHECK(vnmaki::normalization_gain(0.5, 0.1, none) == 1);
  CHECK(vnmaki::normalization_gain(0, 0, peak) == 1);  // silence
  CHECK(vnmaki::normalization_gain(0.5, 0.1, peak) == doctest::Approx(minus_six / 0.5));
  CHECK(vnmaki::normalization_gain(0.5, 0.1, rms) == doctest::Approx(minus_twenty / 0.1));
  // A target that would clip: the peak stops the gain.
  const ImportSettings loud{.normalize = AudioNormalize::kRms, .target_db = 0};
  CHECK(vnmaki::normalization_gain(0.5, 0.1, loud) == doctest::Approx(2));
}

TEST_CASE("audio import: a WAV gets the gain that brings its peak to the target") {
  const ImportSettings settings{.normalize = AudioNormalize::kPeak, .target_db = kPeakTargetDb};
  const vnmaki::AudioImport result =
      vnmaki::import_audio(read_bytes(fixtures() / "tone.wav"), settings);
  const double target = std::pow(10.0, kPeakTargetDb / 20.0);
  CHECK(result.gain == doctest::Approx(target / kTone).epsilon(kGainTolerance));

  REQUIRE(result.opus.size() > 4);
  CHECK(std::string{reinterpret_cast<const char*>(result.opus.data()), 4} == "OggS");
  const Levels levels = levels_of(result.opus);
  CHECK(levels.peak == doctest::Approx(target).epsilon(kOpusTolerance));
  CHECK(static_cast<double>(levels.frames) == doctest::Approx(kHalfSecondFrames).epsilon(0.05));
}

TEST_CASE("audio import: a stereo WAV gets the gain that brings its RMS to the target") {
  const ImportSettings settings{.normalize = AudioNormalize::kRms, .target_db = kRmsTargetDb};
  const vnmaki::AudioImport result =
      vnmaki::import_audio(read_bytes(fixtures() / "quiet.wav"), settings);
  const double rms = kQuiet / kSqrtTwo;  // of a sine
  CHECK(result.gain == doctest::Approx(std::pow(10.0, kRmsTargetDb / 20.0) / rms).epsilon(0.02));
  CHECK(db(levels_of(result.opus).rms) == doctest::Approx(kRmsTargetDb).epsilon(0.05));
}

TEST_CASE("audio import: no normalization keeps the level") {
  const vnmaki::AudioImport result =
      vnmaki::import_audio(read_bytes(fixtures() / "tone.wav"), ImportSettings{});
  CHECK(result.gain == 1);
  CHECK(levels_of(result.opus).peak == doctest::Approx(kTone).epsilon(kOpusTolerance));
}

TEST_CASE("audio import: data that is not audio is an error") {
  const std::vector<std::byte> junk(64, std::byte{0x55});
  CHECK_THROWS_AS(static_cast<void>(vnmaki::import_audio(junk, ImportSettings{})),
                  vnmaki::AudioError);
}

TEST_CASE("import settings: the keys of [build.audio]") {
  CHECK(vnmaki::parse_import_settings("title = \"x\"\n") == ImportSettings{});
  const ImportSettings peak =
      vnmaki::parse_import_settings("[build.audio]\nnormalize = \"peak\"\n");
  CHECK(peak == ImportSettings{.normalize = AudioNormalize::kPeak,
                               .target_db = vnmaki::kDefaultPeakTargetDb});
  const ImportSettings rms =
      vnmaki::parse_import_settings("[build.audio]\nnormalize = \"rms\"\ntarget_db = -14\n");
  CHECK(rms == ImportSettings{.normalize = AudioNormalize::kRms, .target_db = -14});
  CHECK_THROWS_AS(
      static_cast<void>(vnmaki::parse_import_settings("[build.audio]\nnormalize = \"loud\"\n")),
      vnmaki::ImportSettingsError);
  CHECK_THROWS_AS(
      static_cast<void>(vnmaki::parse_import_settings("[build.audio]\ntarget_db = 3\n")),
      vnmaki::ImportSettingsError);
}

TEST_CASE("font import: a subset keeps the glyphs of the characters only") {
  const std::vector<std::byte> font =
      read_bytes(fixtures() / "project" / "fonts" / "default.ttf");
  const vnmaki::FontSubset ascii = vnmaki::subset_font(font, vnmaki::ascii_codepoints());
  const vnmaki::FontSubset three = vnmaki::subset_font(font, {U'A', U'B', U'C'});
  const vnmaki::FontSubset four = vnmaki::subset_font(font, {U'A', U'B', U'C', U'D'});
  CHECK(three.glyph_count == 4);  // .notdef and the three letters
  CHECK(four.glyph_count == three.glyph_count + 1);
  CHECK(ascii.glyph_count == kAsciiGlyphs + 1);
  CHECK(three.font.size() < ascii.font.size());
  CHECK(ascii.font.size() < font.size());
  // A subset is a font again, and a character that the font lacks adds no glyph.
  CHECK(vnmaki::subset_font(three.font, {U'A', U'あ'}).glyph_count == 2);
}

TEST_CASE("font import: data that is not a font is an error") {
  const std::vector<std::byte> junk(64, std::byte{0x55});
  CHECK_THROWS_AS(static_cast<void>(vnmaki::subset_font(junk, {U'A'})), std::runtime_error);
}

TEST_CASE("font import: a language needs ASCII, the script strings and its translations") {
  vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"project.toml", "title = \"T\"\nlanguages = [\"en\", \"ja\"]\n"},
       {"scripts/main.lua",
        "label(\"start\", function() say(nil, \"caf\\u{E9} \xE2\x82\xAC\") end)\n"},
       {"translations/ja.po",
        "msgid \"\"\nmsgstr \"\"\n\nmsgctxt \"a\"\nmsgid \"x\"\nmsgstr \"\xE3\x81\x82\"\n"}});
  const vnmaki::ProjectConfig config = vnmaki::load_project_config(vfs);
  const std::vector<vnmaki::ScriptSource> scripts = vnmaki::read_scripts(vfs);
  const std::set<char32_t> english = vnmaki::language_codepoints(vfs, config, scripts, "en");
  const std::set<char32_t> japanese = vnmaki::language_codepoints(vfs, config, scripts, "ja");
  for (const char32_t c : vnmaki::ascii_codepoints()) {
    CHECK(english.contains(c));
  }
  CHECK(english.contains(U'é'));  // the \u{E9} escape
  CHECK(english.contains(U'€'));  // a letter written in the script
  CHECK_FALSE(english.contains(U'あ'));
  CHECK(japanese.contains(U'あ'));  // from translations/ja.po
  CHECK(japanese.contains(U'é'));   // a missing translation shows the source text
  CHECK(vnmaki::codepoints_text(english) != vnmaki::codepoints_text(japanese));
}

TEST_CASE("gltf import: a valid file has no problem") {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(fixtures() / "gltf")));
  CHECK(vnmaki::validate_gltf(vfs, "good.gltf").empty());
}

TEST_CASE("gltf import: a missing buffer and a missing texture are errors") {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(fixtures() / "gltf")));
  const std::vector<std::string> problems = vnmaki::validate_gltf(vfs, "missing.gltf");
  REQUIRE(problems.size() == 2);
  CHECK(problems[0] == "missing.gltf: buffer 0 'nothing.bin' not found");
  CHECK(problems[1] == "missing.gltf: texture image 0 'nothing.png' not found");
}

TEST_CASE("gltf import: a short buffer, a bad range, bad JSON and a missing file are errors") {
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(fixtures() / "gltf")));
  for (const char* name : {"short.gltf", "bad_index.gltf", "broken.gltf", "absent.gltf"}) {
    INFO(std::string{name});
    const std::vector<std::string> problems = vnmaki::validate_gltf(vfs, name);
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].starts_with(std::string{name} + ": "));
  }
}

TEST_CASE("precompiled shader: the file form round-trips and bad data is an error") {
  vnmaki::CompiledShader shader;
  shader.spirv = {1, 2, 3};
  shader.dxil = {4, 5};
  shader.samplers = 3;
  shader.uniform_buffers = 1;
  const std::vector<std::byte> bytes = vnmaki::write_precompiled_shader(shader);
  const vnmaki::CompiledShader back = vnmaki::read_precompiled_shader(bytes);
  CHECK(back.spirv == shader.spirv);
  CHECK(back.msl.empty());
  CHECK(back.dxil == shader.dxil);
  CHECK(back.samplers == 3);
  CHECK(back.uniform_buffers == 1);

  CHECK_THROWS_AS(static_cast<void>(vnmaki::read_precompiled_shader({})),
                  vnmaki::ShaderCompileError);
  std::vector<std::byte> wrong = bytes;
  wrong[0] = std::byte{'X'};
  CHECK_THROWS_AS(static_cast<void>(vnmaki::read_precompiled_shader(wrong)),
                  vnmaki::ShaderCompileError);
  const std::vector<std::byte> cut{bytes.begin(), bytes.end() - 1};
  CHECK_THROWS_AS(static_cast<void>(vnmaki::read_precompiled_shader(cut)),
                  vnmaki::ShaderCompileError);
}

namespace {

std::string string_of(const std::vector<std::byte>& bytes) {
  std::string text;
  for (const std::byte byte : bytes) {
    text.push_back(static_cast<char>(byte));
  }
  return text;
}

}  // namespace

TEST_CASE("precompiled shader: a game loads it when the source is not there") {
  vnmaki::CompiledShader shader;
  shader.spirv = {1, 2, 3};
  shader.samplers = 1;
  shader.uniform_buffers = 1;
  const std::string file = string_of(vnmaki::write_precompiled_shader(shader));
  vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"shaders/tint.frag.vnshader", file}, {"shaders/bad.frag.vnshader", "not a shader"}});
  vnmaki::Scene scene{vfs, vnmaki::Size{.width = 64, .height = 48}};
  const std::array<std::string, 1> uniforms{"amount"};
  CHECK(scene.declare_shader("tint", uniforms).empty());
  CHECK(scene.declare_shader("bad", {}).find(
            "shaders/bad.frag.vnshader is not a precompiled shader") != std::string::npos);
  CHECK(scene.declare_shader("absent", {}).find("not found") != std::string::npos);
}

TEST_CASE("import settings: [build.fonts] keep lists fonts that are not subset") {
  CHECK(vnmaki::parse_import_settings("title = \"x\"\n").keep_fonts.empty());
  const ImportSettings settings =
      vnmaki::parse_import_settings("[build.fonts]\nkeep = [\"fonts/a.otf\", \"fonts/b.ttf\"]\n");
  CHECK(settings.keep_fonts == std::vector<std::string>{"fonts/a.otf", "fonts/b.ttf"});
  CHECK_THROWS_AS(static_cast<void>(vnmaki::parse_import_settings("[build.fonts]\nkeep = 3\n")),
                  vnmaki::ImportSettingsError);
  CHECK_THROWS_AS(static_cast<void>(vnmaki::parse_import_settings("[build.fonts]\nkeep = [1]\n")),
                  vnmaki::ImportSettingsError);
}

TEST_CASE("importer: a font of [build.fonts] keep gets no subset") {
  const TempDir temp{"keep_font"};
  fs::copy(fixtures() / "project", temp.path(), fs::copy_options::recursive);
  fs::copy_file(temp.path() / "fonts" / "default.ttf", temp.path() / "fonts" / "reserved.ttf");
  std::ofstream{temp.path() / "project.toml", std::ios::binary | std::ios::app}
      << "\n[build.fonts]\nkeep = [\"fonts/reserved.ttf\"]\n";
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(temp.path())));
  const vnmaki::ImportReport report = vnmaki::Importer{vfs, temp.path()}.import_all();
  std::vector<std::string> subsets;
  bool kept = false;
  for (const vnmaki::ImportedFile& file : report.files) {
    kept = kept || (file.source == "fonts/reserved.ttf" && file.kept && file.path.empty());
    if (file.path.starts_with("fonts/")) {
      subsets.push_back(file.path);
    }
  }
  CHECK(kept);
  CHECK(subsets == std::vector<std::string>{"fonts/default.en.ttf", "fonts/default.ja.ttf"});
}

TEST_CASE("importer: a project is imported, then comes from the cache until a file changes") {
  const TempDir temp{"project"};
  fs::copy(fixtures() / "project", temp.path(), fs::copy_options::recursive);
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(temp.path())));
  const vnmaki::Importer importer{vfs, temp.path()};
  const bool shaders = vnmaki::shader_compiler_available();

  const vnmaki::ImportReport first = importer.import_all();
  std::vector<std::string> outputs;
  for (const vnmaki::ImportedFile& file : first.files) {
    CHECK_FALSE(file.cached);
    outputs.push_back(file.path);
  }
  std::vector<std::string> expected{"audio/tone.opus", "fonts/default.en.ttf",
                                    "fonts/default.ja.ttf", ""};  // "": models/triangle.gltf
  if (shaders) {
    expected.emplace_back("shaders/tint.frag.vnshader");
  }
  CHECK(outputs == expected);
  CHECK(first.problems.size() == (shaders ? 0 : 1));

  // The same project again: every output comes from the cache.
  const vnmaki::ImportReport second = importer.import_all();
  REQUIRE(second.files.size() == first.files.size());
  for (std::size_t i = 0; i < second.files.size(); ++i) {
    if (!second.files[i].path.empty()) {
      CHECK(second.files[i].cached);
      CHECK(second.files[i].data == first.files[i].data);
    }
  }

  // A new WAV: only the audio file misses.
  std::vector<std::byte> wav = read_bytes(temp.path() / "audio" / "tone.wav");
  wav.back() = std::byte{0x55};
  write_bytes(temp.path() / "audio" / "tone.wav", wav);
  const std::vector<std::string> changed{"audio/tone.wav", ".vnmaki/cache/x", "audio/none.wav"};
  const vnmaki::ImportReport third = importer.import_changed(changed);
  REQUIRE(third.files.size() == 1);
  CHECK(third.files[0].path == "audio/tone.opus");
  CHECK_FALSE(third.files[0].cached);
  CHECK(third.files[0].data != first.files[0].data);

  // The gain is part of the key: another target misses.
  std::ofstream{temp.path() / "project.toml", std::ios::trunc}
      << "title = \"Import fixture\"\nlanguages = [\"en\", \"ja\"]\n";
  const std::vector<std::string> audio{"audio/tone.wav"};
  const vnmaki::ImportReport fourth = importer.import_changed(audio);
  REQUIRE(fourth.files.size() == 1);
  CHECK_FALSE(fourth.files[0].cached);
}

TEST_CASE("importer: a changed script imports the fonts again, and only the fonts") {
  const TempDir temp{"fonts"};
  fs::copy(fixtures() / "project", temp.path(), fs::copy_options::recursive);
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(temp.path())));
  const vnmaki::Importer importer{vfs, temp.path()};
  static_cast<void>(importer.import_all());

  const std::vector<std::string> scripts{"scripts/main.lua"};
  const vnmaki::ImportReport again = importer.import_changed(scripts);
  REQUIRE(again.files.size() == 2);  // the font in en and in ja
  CHECK(again.files[0].cached);
  CHECK(again.files[1].cached);
}

TEST_CASE("importer: glTF problems are reported and the other files still import") {
  const TempDir temp{"broken"};
  fs::copy(fixtures() / "project_broken", temp.path(), fs::copy_options::recursive);
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(temp.path())));
  const vnmaki::ImportReport report = vnmaki::Importer{vfs, temp.path()}.import_all();
  CHECK(report.files.empty());
  REQUIRE(report.problems.size() == 2);
  CHECK(report.problems[0] == "models/missing.gltf: buffer 0 'nothing.bin' not found");
}

TEST_CASE("importer: a shader is precompiled, and the result loads without the source") {
  if (!vnmaki::shader_compiler_available()) {
    MESSAGE("no run-time shader compiler in this build or on this machine");
    return;
  }
  const TempDir temp{"shader"};
  fs::copy(fixtures() / "project", temp.path(), fs::copy_options::recursive);
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(temp.path())));
  const std::vector<std::string> changed{"shaders/tint.frag.hlsl"};
  const vnmaki::ImportReport report = vnmaki::Importer{vfs, temp.path()}.import_changed(changed);
  REQUIRE(report.files.size() == 1);
  CHECK(report.problems.empty());
  const vnmaki::ImportedFile& file = report.files[0];
  CHECK(file.path == "shaders/tint.frag.vnshader");

  const vnmaki::CompiledShader shader = vnmaki::read_precompiled_shader(file.data);
  CHECK_FALSE(shader.spirv.empty());
  CHECK(shader.uniform_buffers == 1);

  vnmaki::Vfs game = vnmaki::test::memory_vfs({{file.path, string_of(file.data)}});
  vnmaki::Scene scene{game, vnmaki::Size{.width = 64, .height = 48}};
  const std::array<std::string, 1> uniforms{"amount"};
  CHECK(scene.declare_shader("tint", uniforms).empty());
}

TEST_CASE("precompiled shader: the build's own writer (vnshader_pack.py) makes a file that loads") {
  const fs::path file = fs::path{VNMAKI_PRECOMPILED_DIR} / "shaders" / "sepia.frag.vnshader";
  if (!fs::exists(file)) {
    MESSAGE("only a build without a shader compiler makes this file");
    return;
  }
  constexpr std::uint8_t kSpirvMagicLowByte = 0x03;  // 0x07230203, little-endian
  const vnmaki::CompiledShader shader = vnmaki::read_precompiled_shader(read_bytes(file));
  REQUIRE(shader.spirv.size() > 4);
  CHECK(shader.spirv.front() == kSpirvMagicLowByte);
  CHECK(shader.samplers == vnmaki::kMaxShaderTextures);
  CHECK(shader.uniform_buffers == 1);
  CHECK_FALSE(shader.msl.empty());
#ifndef __APPLE__
  CHECK_FALSE(shader.dxil.empty());
#endif
}
