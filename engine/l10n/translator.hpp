#pragma once

// Runtime languages (docs/SPEC.md section 16). A `Catalog` is the translations of one `.po` file,
// found by the keys that `vnmaki extract-strings` writes. The `Translator` holds the language the
// player chose: its catalog, text direction and style file. The scripts keep the source text
// (snapshots, saves and the backlog never hold a translation), and the code that shows a line or
// a screen asks the translator, so a change of language shows at once and is not a rollback step.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/l10n/po.hpp"
#include "engine/l10n/text_direction.hpp"

namespace vnmaki {

class Vfs;
struct ProjectConfig;

/// The usable entries of a `.po` file: not obsolete, not fuzzy, not empty. A key is
/// `<scope>/<hash>` (see translation_key in extract.hpp); `scope` is a label name or
/// `screen/<name>`.
class Catalog {
 public:
  Catalog() = default;
  explicit Catalog(const PoFile& file);

  /// The translation of `text` said by `speaker` in `scope`, or null. It looks for the key of
  /// `speaker` first. The key holds the speaker as the script wrote it (`eileen`), and the
  /// runtime only has the name of the character (`Eileen`), so it then takes the entry of the
  /// same scope and text whose speaker note matches the name when letters and digits are compared
  /// without case. If several entries have the text and none matches, the first one wins.
  /// A line with no speaker finds only its own key.
  [[nodiscard]] const std::string* find(std::string_view scope, std::string_view speaker,
                                        std::string_view text) const;

  [[nodiscard]] bool empty() const { return by_key_.empty(); }

 private:
  struct Item {
    std::string text{};         // msgid
    std::string translation{};  // msgstr
  };
  struct Candidate {
    std::string speaker{};  // the note `speaker: <token>`
    std::string translation{};
  };

  std::map<std::string, Item, std::less<>> by_key_;
  std::map<std::pair<std::string, std::string>, std::vector<Candidate>> by_text_;
};

class Translator {
 public:
  /// Starts in the source language (the first of `config.languages`), which has no `.po` file.
  /// `vfs` must outlive this.
  Translator(const Vfs& vfs, const ProjectConfig& config);

  /// Switches to `code`, which must be one of the languages of project.toml, and reads
  /// `translations/<code>.po` (none for the source language). Returns why it cannot: an unknown
  /// code changes nothing, a `.po` file that does not parse leaves the language switched with no
  /// translations. An empty string is success. The revision changes.
  [[nodiscard]] std::string set_language(std::string_view code);

  [[nodiscard]] const std::string& language() const { return language_; }
  [[nodiscard]] const std::string& source_language() const { return languages_.front(); }
  [[nodiscard]] const std::vector<std::string>& languages() const { return languages_; }
  [[nodiscard]] TextDirection direction() const { return direction_; }
  /// Whether the language's story text is vertical (the `[vertical]` entry of project.toml).
  [[nodiscard]] bool vertical() const {
    const auto found = vertical_.find(language_);
    return found != vertical_.end() && found->second;
  }
  /// The style override file of the language, or empty: the `[styles]` entry of project.toml, else
  /// `styles/<code>.lua` if it exists.
  [[nodiscard]] std::string style_file() const;

  /// Changes whenever the language or its catalog changes: what shows the translations compares
  /// it to the one it drew.
  [[nodiscard]] std::uint64_t revision() const { return revision_; }

  /// The translation, or `text` if there is none (the source language, a missing or fuzzy entry).
  [[nodiscard]] std::string translate(std::string_view scope, std::string_view speaker,
                                      std::string_view text) const;

  /// Replaces the translations of the current language (the translation editor).
  void set_catalog(Catalog catalog);

 private:
  const Vfs* vfs_;
  std::vector<std::string> languages_;
  std::map<std::string, TextDirection> directions_;
  std::map<std::string, std::string> styles_;
  std::map<std::string, bool> vertical_;
  std::string language_;
  TextDirection direction_ = TextDirection::kLtr;
  Catalog catalog_;
  std::uint64_t revision_ = 0;
};

}  // namespace vnmaki
