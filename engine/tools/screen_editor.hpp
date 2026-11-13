#pragma once

// The screen editor (docs/SPEC.md section 18, dev builds only): select a node of a shown screen,
// see where its table is in the script, and change the literal number, string and boolean fields
// of that table. A save replaces exactly the bytes of the literals that changed; all other bytes
// and the comments stay. Hot reload (engine/tools/hot_reload.hpp) then shows the change.
//
// A node is mapped back to its source by its place in the tree: UiNode::source_index is the
// position of a node among the nodes that its parent's table writes, so a path of such indices
// from the root leads to one ScanNode (engine/tools/script_scan.hpp). The map is checked at every
// step, and a node with no sure source is read-only. The overlay (engine/tools/overlay.hpp)
// draws the window; this class needs no window or GPU.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/tools/script_scan.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

/// A change of the bytes [begin, end) of a text.
struct SpanEdit {
  std::size_t begin = 0;
  std::size_t end = 0;
  /// The bytes that must be in the span now. Another content means the file changed.
  std::string expected{};
  std::string replacement{};
};

struct RewriteResult {
  std::string text{};
  /// Why nothing was done; empty when `text` is the new text.
  std::string problem{};
};

/// Applies the edits to `source`. Every other byte stays. Fails (with a problem and no text) when
/// a span is out of range, two overlap, or a span does not hold its `expected` bytes.
[[nodiscard]] RewriteResult rewrite_spans(std::string_view source, std::vector<SpanEdit> edits);

/// `content` as a Lua string literal in `quote` (' or "): backslash, the quote, line breaks and
/// control characters are escaped; other bytes (UTF-8) stay.
[[nodiscard]] std::string lua_string_literal(std::string_view content, char quote);

struct Literal {
  std::string text{};
  /// Why `value` is no literal of the kind; empty when `text` is the literal.
  std::string problem{};
};

/// The source text of `value` as a literal of `kind`: a string in `quote`, a finite decimal
/// number (`-1.5`, `2e3`), or `true` or `false`.
[[nodiscard]] Literal field_literal(ScanValueKind kind, std::string_view value, char quote);

/// One `screen` definition in a script.
struct ScreenDefinition {
  std::string file{};  ///< The Vfs path, e.g. "scripts/ui.lua".
  ScanScreenTree tree{};
};

/// The table-form screens of the scripts that have a name, in the order of the scripts.
[[nodiscard]] std::vector<ScreenDefinition> scan_screen_definitions(
    std::span<const ScriptSource> scripts);

/// The place of a node in the source of its screen: from the root, the index among the nodes
/// written in each table (UiNode::source_index). Empty is the root.
using SourcePath = std::vector<std::size_t>;

/// The node of `tree` that the node at `path` under `root` stands for. The kind and the number of
/// written children are compared at every step. No node, and the reason, when the screen is not
/// written as a table, a table has computed entries, or the source does not match the tree.
struct SourceMatch {
  const ScanNode* node = nullptr;
  std::string problem{};
};
[[nodiscard]] SourceMatch map_to_source(const ScanScreenTree& tree, const UiNode& root,
                                        std::span<const std::size_t> path);

/// The node at `path` under `root` (see SourcePath), or null.
[[nodiscard]] const UiNode* find_by_source(const UiNode& root, std::span<const std::size_t> path);

/// The node that is on top at `point` (virtual pixels), over the screens of the host, topmost
/// first. Screens named `vnmaki:...` (the banner, the menu) are skipped. A node of the engine (a
/// row of a grid) counts as its parent.
struct PickedNode {
  std::string screen{};
  SourcePath path{};
};
[[nodiscard]] std::optional<PickedNode> pick_node(UiHost& ui, PointF point);

class ScreenEditor {
 public:
  /// A field of the selected node's table.
  struct Field {
    std::string key{};  ///< Empty: the string of `text { "..." }`.
    ScanValueKind kind = ScanValueKind::kComputed;
    /// What the field is in the file now, as text (a string: its content).
    std::string original{};
    /// What the user typed; starts as `original`.
    std::string value{};
    /// The bytes of the literal in the file, quotes included.
    std::string raw{};
    std::size_t begin = 0;
    std::size_t end = 0;
    char quote = '"';
    /// A literal that a save can replace.
    bool editable = false;
    std::uint32_t line = 0;

    [[nodiscard]] bool changed() const { return editable && value != original; }
    /// Empty when `value` is fine for the kind.
    [[nodiscard]] std::string problem() const;
  };

  /// `vfs` is where the scripts are read from (only `scripts/`), `project_dir` the folder that has
  /// it, where a save writes. All must outlive the editor.
  ScreenEditor(const Vfs& vfs, std::filesystem::path project_dir, UiHost& ui);

  /// Reads the scripts again. A selection is found again.
  void rescan();

  /// Selects the node under `point` (virtual pixels). Nothing there: nothing is selected.
  bool select_at(PointF point);
  /// Selects a node of a shown screen by its source path. False (and no selection) if it is not
  /// there.
  bool select(std::string screen, SourcePath path);
  void clear();

  [[nodiscard]] bool has_selection() const { return selected_.has_value(); }
  [[nodiscard]] const std::string& screen_name() const;
  [[nodiscard]] const SourcePath& path() const;
  /// The selected node as laid out now; null when there is none or it is gone (a reload).
  [[nodiscard]] const UiNode* node();
  /// Where the table of the selected node is, e.g. "scripts/ui.lua:12". Empty without a source.
  [[nodiscard]] std::string location() const;
  /// The kind written in the source, e.g. "hbox". Empty without a source.
  [[nodiscard]] const std::string& source_kind() const { return source_kind_; }
  /// Why the selected node has no source, so it is read-only. Empty when it has one.
  [[nodiscard]] const std::string& problem() const { return problem_; }
  [[nodiscard]] std::vector<Field>& fields() { return fields_; }

  /// Puts the typed values back to what the file has.
  void revert();

  /// Writes the changed fields to the script. Returns why it could not, or an empty string (also
  /// when nothing changed). The file is written whole to a temporary file and moved over the
  /// script, so a failed write keeps the old script.
  [[nodiscard]] std::string save();

 private:
  struct Selection {
    std::string screen{};
    SourcePath path{};
  };

  void refresh();

  const Vfs* vfs_;
  std::filesystem::path project_dir_;
  UiHost* ui_;
  std::vector<ScriptSource> scripts_;
  std::vector<ScreenDefinition> definitions_;
  std::optional<Selection> selected_;
  std::string file_;
  std::uint32_t line_ = 0;
  std::string source_kind_;
  std::string problem_;
  std::vector<Field> fields_;
};

}  // namespace vnmaki
