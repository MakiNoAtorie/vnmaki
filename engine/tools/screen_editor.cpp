#include "engine/tools/screen_editor.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/tools/script_scan.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

namespace {

constexpr unsigned char kFirstPrintable = 0x20;
constexpr unsigned char kDelete = 0x7F;
constexpr std::string_view kEngineScreenPrefix = "vnmaki:";
constexpr std::string_view kTemporarySuffix = ".vnmaki-tmp";
// A `\u{...}` escape stays as written in a decoded string (the scan), so such a string cannot be
// written back from its content.
constexpr std::string_view kUnicodeEscape = "\\u{";

std::optional<NodeKind> ui_kind(std::string_view kind) {
  struct Entry {
    std::string_view name{};
    NodeKind kind = NodeKind::kBox;
  };
  static constexpr std::array<Entry, 11> kEntries{{
      {.name = "box", .kind = NodeKind::kBox},
      {.name = "hbox", .kind = NodeKind::kHbox},
      {.name = "vbox", .kind = NodeKind::kVbox},
      {.name = "text", .kind = NodeKind::kText},
      {.name = "image", .kind = NodeKind::kImage},
      {.name = "button", .kind = NodeKind::kButton},
      {.name = "toggle", .kind = NodeKind::kToggle},
      {.name = "slider", .kind = NodeKind::kSlider},
      {.name = "bar", .kind = NodeKind::kBar},
      {.name = "grid", .kind = NodeKind::kVbox},  // a grid is a column of rows
      {.name = "input", .kind = NodeKind::kInput},
  }};
  for (const Entry& entry : kEntries) {
    if (entry.name == kind) {
      return entry.kind;
    }
  }
  return std::nullopt;
}

// The nodes that the script wrote directly under `node`: the nodes that the engine added (the
// rows of a grid) are looked through.
std::size_t written_children(const UiNode& node) {
  std::size_t count = 0;
  for (const UiNode& child : node.children) {
    count += child.source_index >= 0 ? 1 : written_children(child);
  }
  return count;
}

const UiNode* find_written_child(const UiNode& parent, std::size_t index) {
  for (const UiNode& child : parent.children) {
    if (child.source_index >= 0) {
      if (static_cast<std::size_t>(child.source_index) == index) {
        return &child;
      }
    } else if (const UiNode* found = find_written_child(child, index)) {
      return found;
    }
  }
  return nullptr;
}

// Whether the node on screen can be the node in the source.
std::string check_node(const UiNode& ui, const ScanNode& node) {
  const std::optional<NodeKind> kind = ui_kind(node.kind);
  if (!kind || *kind != ui.kind) {
    return "the screen has another kind of node here than the source (" + node.kind + ")";
  }
  if (!node.dynamic && written_children(ui) != node.children.size()) {
    return "the source writes " + std::to_string(node.children.size()) +
           " nodes in this table, the screen has " + std::to_string(written_children(ui));
  }
  return {};
}

bool contains(const RectF& rect, PointF point) {
  return point.x >= rect.x && point.y >= rect.y && point.x < rect.x + rect.width &&
         point.y < rect.y + rect.height;
}

// Whether `point` hits `node` or a node under it. `path` gets the source indices of the written
// nodes from `node` to the one hit. The last child is on top.
bool pick_in(const UiNode& node, PointF point, SourcePath& path) {
  for (auto child = node.children.rbegin(); child != node.children.rend(); ++child) {
    const bool written = child->source_index >= 0;
    if (written) {
      path.push_back(static_cast<std::size_t>(child->source_index));
    }
    if (pick_in(*child, point, path)) {
      return true;
    }
    if (written) {
      path.pop_back();
    }
  }
  return contains(node.rect, point);
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream file{path, std::ios::binary};
  return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

}  // namespace

RewriteResult rewrite_spans(std::string_view source, std::vector<SpanEdit> edits) {
  std::ranges::sort(edits, {}, &SpanEdit::begin);
  RewriteResult result;
  result.text.reserve(source.size());
  std::size_t at = 0;
  for (const SpanEdit& edit : edits) {
    if (edit.begin < at || edit.end < edit.begin || edit.end > source.size()) {
      return {.problem = "an edit is outside the file or overlaps another"};
    }
    if (source.substr(edit.begin, edit.end - edit.begin) != edit.expected) {
      return {.problem = "the file changed since the node was selected"};
    }
    result.text.append(source.substr(at, edit.begin - at));
    result.text += edit.replacement;
    at = edit.end;
  }
  result.text.append(source.substr(at));
  return result;
}

std::string lua_string_literal(std::string_view content, char quote) {
  std::string text(1, quote);
  text.reserve(content.size() + 2);
  for (const char c : content) {
    const auto byte = static_cast<unsigned char>(c);
    if (c == '\\' || c == quote) {
      text.push_back('\\');
      text.push_back(c);
    } else if (c == '\n') {
      text += "\\n";
    } else if (c == '\r') {
      text += "\\r";
    } else if (byte < kFirstPrintable || byte == kDelete) {
      text += std::format("\\{:03}", static_cast<unsigned>(byte));  // 3 digits: a digit may follow
    } else {
      text.push_back(c);
    }
  }
  text.push_back(quote);
  return text;
}

Literal field_literal(ScanValueKind kind, std::string_view value, char quote) {
  switch (kind) {
    case ScanValueKind::kString:
      return {.text = lua_string_literal(value, quote)};
    case ScanValueKind::kBoolean:
      if (value == "true" || value == "false") {
        return {.text = std::string{value}};
      }
      return {.problem = "must be true or false"};
    case ScanValueKind::kNumber: {
      double number = 0;
      const char* const last = value.data() + value.size();
      const auto [stop, error] = std::from_chars(value.data(), last, number);
      if (value.empty() || error != std::errc{} || stop != last || !std::isfinite(number)) {
        return {.problem = "must be a decimal number"};
      }
      return {.text = std::string{value}};
    }
    case ScanValueKind::kComputed:
      break;
  }
  return {.problem = "a computed value is read-only"};
}

std::vector<ScreenDefinition> scan_screen_definitions(std::span<const ScriptSource> scripts) {
  std::vector<ScreenDefinition> definitions;
  for (const ScriptSource& script : scripts) {
    for (ScanScreenTree& tree : scan_screen_trees(script.text)) {
      if (tree.name) {
        definitions.push_back({.file = script.path, .tree = std::move(tree)});
      }
    }
  }
  return definitions;
}

SourceMatch map_to_source(const ScanScreenTree& tree, const UiNode& root,
                          std::span<const std::size_t> path) {
  if (!tree.root) {
    return {.problem = "the screen is a function or has a computed name"};
  }
  const ScanNode* node = &*tree.root;
  const UiNode* ui = &root;
  if (std::string problem = check_node(*ui, *node); !problem.empty()) {
    return {.problem = std::move(problem)};
  }
  for (const std::size_t index : path) {
    if (node->dynamic) {
      return {.problem = "a table above has computed entries, so its nodes are not mapped"};
    }
    ui = index < node->children.size() ? find_written_child(*ui, index) : nullptr;
    if (ui == nullptr) {
      return {.problem = "the node is not in the source"};
    }
    node = &node->children[index];
    if (std::string problem = check_node(*ui, *node); !problem.empty()) {
      return {.problem = std::move(problem)};
    }
  }
  return {.node = node};
}

const UiNode* find_by_source(const UiNode& root, std::span<const std::size_t> path) {
  const UiNode* node = &root;
  for (const std::size_t index : path) {
    node = find_written_child(*node, index);
    if (node == nullptr) {
      return nullptr;
    }
  }
  return node;
}

std::optional<PickedNode> pick_node(UiHost& ui, PointF point) {
  const std::vector<std::string> names = ui.screen_names();
  for (auto name = names.rbegin(); name != names.rend(); ++name) {
    const UiNode* root = name->starts_with(kEngineScreenPrefix) ? nullptr : ui.find(*name);
    SourcePath path;
    if (root != nullptr && pick_in(*root, point, path)) {
      return PickedNode{.screen = *name, .path = std::move(path)};
    }
  }
  return std::nullopt;
}

std::string ScreenEditor::Field::problem() const {
  return editable ? field_literal(kind, value, quote).problem : std::string{};
}

ScreenEditor::ScreenEditor(const Vfs& vfs, std::filesystem::path project_dir, UiHost& ui)
    : vfs_(&vfs), project_dir_(std::move(project_dir)), ui_(&ui) {
  rescan();
}

void ScreenEditor::rescan() {
  scripts_ = read_scripts(*vfs_);
  definitions_ = scan_screen_definitions(scripts_);
  if (selected_) {
    refresh();
  }
}

bool ScreenEditor::select_at(PointF point) {
  std::optional<PickedNode> picked = pick_node(*ui_, point);
  if (!picked) {
    clear();
    return false;
  }
  return select(std::move(picked->screen), std::move(picked->path));
}

bool ScreenEditor::select(std::string screen, SourcePath path) {
  const UiNode* root = ui_->find(screen);
  if (root == nullptr || find_by_source(*root, path) == nullptr) {
    clear();
    return false;
  }
  selected_ = Selection{.screen = std::move(screen), .path = std::move(path)};
  rescan();  // the file may have changed since the last look
  return true;
}

void ScreenEditor::clear() {
  selected_.reset();
  fields_.clear();
  problem_.clear();
  source_kind_.clear();
  file_.clear();
}

const std::string& ScreenEditor::screen_name() const {
  static const std::string kNone;
  return selected_ ? selected_->screen : kNone;
}

const SourcePath& ScreenEditor::path() const {
  static const SourcePath kNone;
  return selected_ ? selected_->path : kNone;
}

const UiNode* ScreenEditor::node() {
  if (!selected_) {
    return nullptr;
  }
  const UiNode* root = ui_->find(selected_->screen);
  return root == nullptr ? nullptr : find_by_source(*root, selected_->path);
}

std::string ScreenEditor::location() const {
  return source_kind_.empty() ? std::string{} : file_ + ":" + std::to_string(line_);
}

void ScreenEditor::revert() {
  for (Field& field : fields_) {
    field.value = field.original;
  }
}

// Finds the selected node in the source again and takes its fields.
void ScreenEditor::refresh() {
  fields_.clear();
  problem_.clear();
  source_kind_.clear();
  file_.clear();
  const UiNode* root = ui_->find(selected_->screen);
  if (root == nullptr) {
    problem_ = "the screen is not shown";
    return;
  }
  const ScreenDefinition* found = nullptr;
  for (const ScreenDefinition& definition : definitions_) {
    if (definition.tree.name == selected_->screen) {
      if (found != nullptr) {
        problem_ = "the screen is defined more than once in the scripts";
        return;
      }
      found = &definition;
    }
  }
  if (found == nullptr) {
    problem_ = "no script defines the screen as a table (the engine, or a function, makes it)";
    return;
  }
  const SourceMatch match = map_to_source(found->tree, *root, selected_->path);
  if (match.node == nullptr) {
    problem_ = match.problem;
    return;
  }
  const auto script = std::ranges::find(scripts_, found->file, &ScriptSource::path);
  if (script == scripts_.end()) {
    problem_ = "the script is gone";
    return;
  }
  file_ = found->file;
  line_ = match.node->line;
  source_kind_ = match.node->kind;
  for (const ScanField& scanned : match.node->fields) {
    Field field{.key = scanned.key,
                .kind = scanned.kind,
                .original = scanned.text,
                .raw = script->text.substr(scanned.begin, scanned.end - scanned.begin),
                .begin = scanned.begin,
                .end = scanned.end,
                .line = scanned.line};
    if (scanned.kind == ScanValueKind::kComputed) {
      field.original = field.raw;
    } else {
      field.editable = field.raw.find(kUnicodeEscape) == std::string::npos;
    }
    if (scanned.kind == ScanValueKind::kString &&
        (field.raw.starts_with('\'') || field.raw.starts_with('"'))) {
      field.quote = field.raw.front();
    }
    field.value = field.original;
    fields_.push_back(std::move(field));
  }
}

std::string ScreenEditor::save() {
  if (!selected_) {
    return "no node is selected";
  }
  if (!problem_.empty()) {
    return "read-only: " + problem_;
  }
  std::vector<SpanEdit> edits;
  for (const Field& field : fields_) {
    if (!field.changed()) {
      continue;
    }
    const Literal literal = field_literal(field.kind, field.value, field.quote);
    if (!literal.problem.empty()) {
      return "'" + (field.key.empty() ? std::string{"text"} : field.key) + "' " + literal.problem;
    }
    edits.push_back({.begin = field.begin,
                     .end = field.end,
                     .expected = field.raw,
                     .replacement = literal.text});
  }
  if (edits.empty()) {
    return {};
  }
  const std::filesystem::path path = project_dir_ / std::filesystem::path{file_};
  const RewriteResult rewritten = rewrite_spans(read_file(path), std::move(edits));
  if (!rewritten.problem.empty()) {
    return rewritten.problem + " (" + file_ + ")";
  }
  std::filesystem::path temporary = path;
  temporary += kTemporarySuffix;
  {
    std::ofstream out{temporary, std::ios::binary | std::ios::trunc};
    out.write(rewritten.text.data(), static_cast<std::streamsize>(rewritten.text.size()));
    out.close();
    if (!out) {
      return "cannot write " + temporary.string();
    }
  }
  std::error_code error;
  std::filesystem::rename(temporary, path, error);
  if (error) {
    std::filesystem::remove(temporary, error);
    return "cannot replace " + file_;
  }
  rescan();
  return {};
}

}  // namespace vnmaki
