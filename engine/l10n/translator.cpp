#include "engine/l10n/translator.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/l10n/extract.hpp"
#include "engine/runtime/project_config.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kSpeakerNote = "speaker: ";

// Letters and digits in lower case: "Mr. Smith" and `mr_smith` match.
std::string normalized(std::string_view name) {
  std::string result;
  result.reserve(name.size());
  for (const char c : name) {
    if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
      result.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
  }
  return result;
}

// The speaker as the script wrote it: `eileen`, or `chars.eileen` (then the part after the dot,
// because the character's name is what the runtime knows).
std::string speaker_token(const PoEntry& entry) {
  for (const std::string& note : entry.extracted) {
    if (note.starts_with(kSpeakerNote)) {
      return note.substr(kSpeakerNote.size());
    }
  }
  return {};
}

std::string scope_of(const std::string& context) { return context.substr(0, context.rfind('/')); }

}  // namespace

Catalog::Catalog(const PoFile& file) {
  for (const PoEntry& entry : file.entries) {
    if (entry.obsolete || entry.fuzzy() || entry.str.empty() || entry.id.empty() ||
        entry.context.empty()) {
      continue;
    }
    by_key_.insert_or_assign(entry.context, Item{.text = entry.id, .translation = entry.str});
    by_text_[{scope_of(entry.context), entry.id}].push_back(
        {.speaker = speaker_token(entry), .translation = entry.str});
  }
}

const std::string* Catalog::find(std::string_view scope, std::string_view speaker,
                                 std::string_view text) const {
  if (by_key_.empty()) {
    return nullptr;
  }
  const auto exact = by_key_.find(translation_key(scope, speaker, text));
  if (exact != by_key_.end() && exact->second.text == text) {
    return &exact->second.translation;
  }
  if (speaker.empty()) {
    return nullptr;
  }
  const auto candidates = by_text_.find({std::string{scope}, std::string{text}});
  if (candidates == by_text_.end()) {
    return nullptr;
  }
  const std::string wanted = normalized(speaker);
  for (const Candidate& candidate : candidates->second) {
    const std::size_t dot = candidate.speaker.rfind('.');
    const std::string_view last = dot == std::string::npos
                                      ? std::string_view{candidate.speaker}
                                      : std::string_view{candidate.speaker}.substr(dot + 1);
    if (!candidate.speaker.empty() &&
        (normalized(candidate.speaker) == wanted || normalized(last) == wanted)) {
      return &candidate.translation;
    }
  }
  // ponytail: two speakers with the same text in one label and no name match take the first.
  return &candidates->second.front().translation;
}

Translator::Translator(const Vfs& vfs, const ProjectConfig& config)
    : vfs_(&vfs),
      languages_(config.languages),
      directions_(config.text_directions.begin(), config.text_directions.end()),
      styles_(config.language_styles),
      language_(config.languages.front()) {
  if (const auto found = directions_.find(language_); found != directions_.end()) {
    direction_ = found->second;
  }
}

std::string Translator::set_language(std::string_view code) {
  if (std::ranges::find(languages_, code) == languages_.end()) {
    return "language '" + std::string{code} + "' is not in 'languages' of project.toml";
  }
  language_ = std::string{code};
  const auto direction = directions_.find(language_);
  direction_ = direction != directions_.end() ? direction->second : TextDirection::kLtr;
  catalog_ = {};
  std::string problem;
  if (language_ != source_language()) {
    const std::string path = translation_path(language_);
    if (const std::optional<std::vector<std::byte>> bytes = vfs_->read(path)) {
      std::string text;
      text.reserve(bytes->size());
      for (const std::byte b : *bytes) {
        text.push_back(static_cast<char>(b));
      }
      try {
        catalog_ = Catalog{parse_po(text)};
      } catch (const PoError& error) {
        problem = path + ":" + std::to_string(error.line()) + ": " + error.what();
      }
    }
  }
  ++revision_;
  return problem;
}

std::string Translator::style_file() const {
  if (const auto found = styles_.find(language_); found != styles_.end()) {
    return found->second;
  }
  std::string path = "styles/" + language_ + ".lua";
  return vfs_->exists(path) ? path : std::string{};
}

std::string Translator::translate(std::string_view scope, std::string_view speaker,
                                  std::string_view text) const {
  if (const std::string* found = catalog_.find(scope, speaker, text)) {
    return *found;
  }
  return std::string{text};
}

void Translator::set_catalog(Catalog catalog) {
  catalog_ = std::move(catalog);
  ++revision_;
}

}  // namespace vnmaki
