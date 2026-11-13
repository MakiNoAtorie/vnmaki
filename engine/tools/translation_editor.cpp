#include "engine/tools/translation_editor.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/l10n/extract.hpp"
#include "engine/text/markup.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kSpeakerNote = "speaker: ";
constexpr std::string_view kScreenScope = "screen/";

std::string scope_of(const std::string& context) { return context.substr(0, context.rfind('/')); }

bool is_header(const PoEntry& entry) { return entry.context.empty() && entry.id.empty(); }

// Writes next to the file and renames, so that a failed write keeps the old translations.
std::string write_file(const std::filesystem::path& path, const std::string& text) {
  std::filesystem::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
    out << text;
    if (!out.flush()) {
      return "cannot write " + temporary.string();
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  return error ? "cannot write " + path.string() + ": " + error.message() : std::string{};
}

}  // namespace

TranslationEditor::TranslationEditor(Translator& translator, const ScriptRuntime& runtime,
                                     std::filesystem::path project_dir)
    : translator_(&translator), runtime_(&runtime), project_dir_(std::move(project_dir)) {}

bool TranslationEditor::load() {
  if (loaded_language_ == translator_->language()) {
    return true;
  }
  file_ = {};
  loaded_language_.clear();
  problem_.clear();
  if (!editable()) {
    return false;
  }
  const std::string relative = translation_path(translator_->language());
  std::ifstream in{project_dir_ / relative, std::ios::binary};
  if (!in) {
    problem_ = relative + " does not exist (run vnmaki extract-strings)";
    return false;
  }
  const std::string text{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
  try {
    file_ = parse_po(text);
  } catch (const PoError& error) {
    problem_ = relative + ":" + std::to_string(error.line()) + ": " + error.what();
    return false;
  }
  loaded_language_ = translator_->language();
  return true;
}

std::vector<TranslationRow> TranslationEditor::rows() {
  std::vector<TranslationRow> result;
  if (!load()) {
    return result;
  }
  const std::string label = runtime_->position().label;
  for (const PoEntry& entry : file_.entries) {
    if (is_header(entry) || entry.obsolete || scope_of(entry.context) != label) {
      continue;
    }
    TranslationRow row{
        .key = entry.context, .source = entry.id, .translation = entry.str, .fuzzy = entry.fuzzy()};
    for (const std::string& note : entry.extracted) {
      if (note.starts_with(kSpeakerNote)) {
        row.speaker = note.substr(kSpeakerNote.size());
      }
    }
    result.push_back(std::move(row));
  }
  return result;
}

std::string TranslationEditor::edit(const std::string& key, const std::string& text) {
  // Read the file again: Poedit or extract-strings may have changed it since rows(), and writing
  // the cached copy would undo that.
  loaded_language_.clear();
  if (!load()) {
    return problem_.empty() ? "the source language has no translations" : problem_;
  }
  const auto found = std::ranges::find_if(file_.entries, [&](const PoEntry& entry) {
    return !is_header(entry) && !entry.obsolete && entry.context == key;
  });
  if (found == file_.entries.end()) {
    return "no entry '" + key + "'";
  }
  if (!scope_of(key).starts_with(kScreenScope)) {
    try {
      static_cast<void>(parse_markup(text));
    } catch (const MarkupError& error) {
      return std::string{"the text tags do not parse: "} + error.what();
    }
  }
  const PoEntry before = *found;
  found->str = text;
  found->set_fuzzy(false);
  const std::string path = translation_path(translator_->language());
  if (std::string problem = write_file(project_dir_ / path, write_po(file_)); !problem.empty()) {
    *found = before;
    problem_ = problem;
    return problem;
  }
  problem_.clear();
  translator_->set_catalog(Catalog{file_});
  return {};
}

}  // namespace vnmaki
