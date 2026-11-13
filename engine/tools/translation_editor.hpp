#pragma once

// The translation editor of the dev overlay (docs/SPEC.md section 16). It lists the entries of
// the label the story is in, in the language that is shown, and saves an edit to the `.po` file of
// the project. It uses no ImGui: the overlay draws the rows.

#include <filesystem>
#include <string>
#include <vector>

#include "engine/l10n/po.hpp"
#include "engine/l10n/translator.hpp"
#include "engine/runtime/script_runtime.hpp"

namespace vnmaki {

/// One entry of the label. An entry that was never translated has an empty `translation`.
struct TranslationRow {
  std::string key{};      ///< The `msgctxt`.
  std::string speaker{};  ///< From the note `speaker: ...`; empty for narration, menus, screens.
  std::string source{};   ///< The `msgid`.
  std::string translation{};
  bool fuzzy = false;

  friend bool operator==(const TranslationRow&, const TranslationRow&) = default;
};

class TranslationEditor {
 public:
  /// `translator` and `runtime` must outlive this. `project_dir` is the folder whose
  /// `translations/<language>.po` files the editor reads and writes (the VFS is read-only).
  TranslationEditor(Translator& translator, const ScriptRuntime& runtime,
                    std::filesystem::path project_dir);

  /// The language that is shown, and whether it has a `.po` file to edit (the source language has
  /// none).
  [[nodiscard]] const std::string& language() const { return translator_->language(); }
  [[nodiscard]] bool editable() const {
    return translator_->language() != translator_->source_language();
  }

  /// The entries of the label that the story is in now (not the obsolete ones), in the order of
  /// the file. It reads the file again when the language is not the one it read. Empty for the
  /// source language, when the file is missing or when it cannot be read; `problem()` says why.
  [[nodiscard]] std::vector<TranslationRow> rows();
  /// Why the last rows() or edit() failed, or an empty string.
  [[nodiscard]] const std::string& problem() const { return problem_; }

  /// Sets the translation of entry `key` (an empty text removes it, so the source text shows),
  /// takes its fuzzy flag off, writes the `.po` file and gives the translator the new
  /// translations, so the say box, the backlog and the screens change at once. A text of a line
  /// whose tags do not parse is refused (as `vnmaki check` would). Returns why it cannot, or an
  /// empty string; then nothing changed.
  [[nodiscard]] std::string edit(const std::string& key, const std::string& text);

 private:
  bool load();

  Translator* translator_;
  const ScriptRuntime* runtime_;
  std::filesystem::path project_dir_;
  PoFile file_{};
  std::string loaded_language_{};
  std::string problem_{};
};

}  // namespace vnmaki
