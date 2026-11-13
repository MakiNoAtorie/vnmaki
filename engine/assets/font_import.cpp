#include "engine/assets/font_import.hpp"

#include <charconv>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <hb-subset.h>
#include <hb.h>

#include "engine/l10n/extract.hpp"
#include "engine/l10n/po.hpp"
#include "engine/tools/script_scan.hpp"

namespace vnmaki {

namespace {

constexpr char32_t kFirstAscii = 0x20;
constexpr char32_t kLastAscii = 0x7E;
constexpr char32_t kMaxCodepoint = 0x10FFFF;
constexpr int kHexBase = 16;

template <class T, void (*Destroy)(T*)>
struct HbDeleter {
  void operator()(T* object) const { Destroy(object); }
};
using Blob = std::unique_ptr<hb_blob_t, HbDeleter<hb_blob_t, hb_blob_destroy>>;
using Face = std::unique_ptr<hb_face_t, HbDeleter<hb_face_t, hb_face_destroy>>;
using Buffer = std::unique_ptr<hb_buffer_t, HbDeleter<hb_buffer_t, hb_buffer_destroy>>;
using SubsetInput =
    std::unique_ptr<hb_subset_input_t, HbDeleter<hb_subset_input_t, hb_subset_input_destroy>>;

// The codepoints of UTF-8 text (HarfBuzz decodes it; a bad byte gives U+FFFD). Controls are
// skipped.
void add_utf8(std::set<char32_t>& codepoints, std::string_view text) {
  const Buffer buffer{hb_buffer_create()};
  hb_buffer_add_utf8(buffer.get(), text.data(), static_cast<int>(text.size()), 0,
                     static_cast<int>(text.size()));
  unsigned count = 0;
  const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer.get(), &count);
  for (unsigned i = 0; i < count; ++i) {
    if (infos[i].codepoint >= kFirstAscii) {
      codepoints.insert(infos[i].codepoint);
    }
  }
}

// A Lua string token keeps `\u{XXXX}` as written (engine/tools/script_scan.hpp), so decode it here.
void add_lua_string(std::set<char32_t>& codepoints, std::string_view text) {
  constexpr std::string_view opener = "\\u{";
  add_utf8(codepoints, text);
  for (std::size_t at = text.find(opener); at != std::string_view::npos;
       at = text.find(opener, at + 1)) {
    const std::size_t digits = at + opener.size();
    const std::size_t close = text.find('}', digits);
    if (close == std::string_view::npos) {
      break;
    }
    std::uint32_t value = 0;
    const auto [end, error] =
        std::from_chars(text.data() + digits, text.data() + close, value, kHexBase);
    if (error == std::errc{} && end == text.data() + close && value >= kFirstAscii &&
        value <= kMaxCodepoint) {
      codepoints.insert(value);
    }
  }
}

}  // namespace

std::set<char32_t> ascii_codepoints() {
  std::set<char32_t> codepoints;
  for (char32_t c = kFirstAscii; c <= kLastAscii; ++c) {
    codepoints.insert(c);
  }
  return codepoints;
}

std::set<char32_t> language_codepoints(const Vfs& vfs, const ProjectConfig& config,
                                       std::span<const ScriptSource> scripts,
                                       const std::string& language) {
  std::set<char32_t> codepoints = ascii_codepoints();
  add_utf8(codepoints, config.title);
  for (const ScriptSource& script : scripts) {
    for (const LuaToken& token : tokenize_lua(script.text)) {
      if (token.kind == LuaTokenKind::kString) {
        add_lua_string(codepoints, token.text);
      }
    }
  }
  if (!config.languages.empty() && language == config.languages.front()) {
    return codepoints;  // the source language has no .po file
  }
  const std::string path = translation_path(language);
  if (const auto bytes = vfs.read(path)) {
    std::string text;
    text.reserve(bytes->size());
    for (const std::byte byte : *bytes) {
      text.push_back(static_cast<char>(byte));
    }
    try {
      for (const PoEntry& entry : parse_po(text).entries) {
        if (!entry.obsolete) {
          add_utf8(codepoints, entry.str);
        }
      }
    } catch (const PoError& error) {
      throw std::runtime_error{path + ":" + std::to_string(error.line()) + ": " + error.what()};
    }
  }
  return codepoints;
}

std::string codepoints_text(const std::set<char32_t>& codepoints) {
  std::string text;
  for (const char32_t value : codepoints) {
    text += std::to_string(static_cast<std::uint32_t>(value)) + ",";
  }
  return text;
}

FontSubset subset_font(std::span<const std::byte> font, const std::set<char32_t>& codepoints) {
  const Blob blob{hb_blob_create(reinterpret_cast<const char*>(font.data()),
                                 static_cast<unsigned>(font.size()), HB_MEMORY_MODE_READONLY,
                                 nullptr, nullptr)};
  const Face face{hb_face_create(blob.get(), 0)};
  if (hb_face_get_glyph_count(face.get()) == 0) {
    throw std::runtime_error{"is not a TrueType or OpenType font"};
  }
  const SubsetInput input{hb_subset_input_create_or_fail()};
  if (!input) {
    throw std::runtime_error{"cannot subset the font: out of memory"};
  }
  hb_set_t* unicodes = hb_subset_input_unicode_set(input.get());
  for (const char32_t value : codepoints) {
    hb_set_add(unicodes, value);
  }
  // A box for a missing glyph, as in the full font.
  hb_subset_input_set_flags(input.get(), HB_SUBSET_FLAGS_NOTDEF_OUTLINE);
  const Face subset{hb_subset_or_fail(face.get(), input.get())};
  if (!subset) {
    throw std::runtime_error{"cannot subset the font"};
  }
  const Blob data{hb_face_reference_blob(subset.get())};
  unsigned length = 0;
  const char* bytes = hb_blob_get_data(data.get(), &length);
  FontSubset result;
  result.glyph_count = hb_face_get_glyph_count(subset.get());
  result.font.reserve(length);
  for (unsigned i = 0; i < length; ++i) {
    result.font.push_back(static_cast<std::byte>(bytes[i]));
  }
  return result;
}

}  // namespace vnmaki
