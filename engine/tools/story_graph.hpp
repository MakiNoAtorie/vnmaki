#pragma once

// The story graph of docs/SPEC.md section 18: the labels of a game and the `jump`, `call` and
// `menu` edges between them, from a scan of the scripts (script_scan.hpp) and from the labels that
// a run entered (TraceRecorder). It shows labels that `start` cannot reach and labels that end the
// story by accident. Nothing here runs a script, and nothing needs ImGui: the overlay panel and
// `vnmaki graph` both read a StoryGraph.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "engine/runtime/script_runtime.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

/// The label where the story starts.
inline constexpr std::string_view kStartLabel = "start";

struct ScriptSource {
  std::string path{};  ///< The Vfs path, e.g. "scripts/main.lua".
  std::string text{};
};

enum class EdgeKind : std::uint8_t {
  kJump,
  kCall,
  /// A `jump` or `call` in a block that opens after a `menu` call of the same label, so it runs
  /// for the answer of that menu. It does not say which of the two it is.
  kMenu,
  /// Seen in a run, and not in the scripts: the target is computed.
  kTrace,
};

struct GraphLabel {
  std::string name{};
  std::string script{};
  std::uint32_t line = 0;
  /// The label call has the third argument `{ ending = true }`.
  bool ending = false;
  /// A path from `start` reaches it, over the edges of the scripts and the traced edges.
  bool reachable = false;
  /// It has no `jump` and no `ret`, is not marked `ending`, and no `call` names it (a called
  /// label that runs to its end returns to its caller). A branch that runs to the end while
  /// another branch jumps is not found.
  bool dead_end = false;
  /// A run entered it.
  bool seen = false;
};

struct GraphEdge {
  std::string from{};
  std::string to{};  ///< Empty for a computed target.
  EdgeKind kind = EdgeKind::kJump;
  bool dynamic = false;  ///< The target is not a string literal.
  std::string script{};  ///< Empty for a traced edge.
  std::uint32_t line = 0;
  /// A run took it.
  bool seen = false;

  friend bool operator==(const GraphEdge&, const GraphEdge&) = default;
};

/// `labels` are in source order (scripts in the order given), `edges` in the order of their
/// labels and then of the source. A label defined twice counts once, the first time.
struct StoryGraph {
  std::vector<GraphLabel> labels{};
  std::vector<GraphEdge> edges{};
};

/// The `.lua` files under `scripts/` of `vfs`, sorted by path, as the runtime loads them.
[[nodiscard]] std::vector<ScriptSource> read_scripts(const Vfs& vfs);

/// Scans the scripts. The graph has no traced edges.
[[nodiscard]] StoryGraph build_story_graph(std::span<const ScriptSource> scripts);

/// A label entered through a `jump` or a `call` of another label.
struct TraceEdge {
  std::string from{};
  std::string to{};

  friend bool operator==(const TraceEdge&, const TraceEdge&) = default;
};

/// Collects the labels and the edges of a run. Set it as `ScriptRuntimeOptions::observer` (or call
/// it from the observer that is set). A replay is not reported label by label; `on_replay()` calls
/// `break_trace()`, so a load or a rollback gives no false edge from the label that ran before.
class TraceRecorder final : public ScriptObserver {
 public:
  void on_label_entered(std::string_view label) override;
  void on_label_resumed(std::string_view label) override;
  /// A load or a rollback starts: the next label entered has no edge into it.
  void on_replay() override { break_trace(); }

  /// The next label entered has no edge into it.
  void break_trace() { current_.clear(); }
  void clear();

  /// In the order of the first time. Each edge once.
  [[nodiscard]] const std::vector<TraceEdge>& edges() const { return edges_; }
  /// The labels entered, in the order of the first time. Each label once.
  [[nodiscard]] const std::vector<std::string>& labels() const { return labels_; }
  /// Changes each time a label or an edge is added and when `clear()` runs: a panel that shows
  /// the trace applies it again when this number is not the one it saw.
  [[nodiscard]] std::size_t revision() const { return revision_; }

 private:
  std::vector<TraceEdge> edges_;
  std::vector<std::string> labels_;
  std::string current_;  ///< The label that runs now ("" before the first).
  std::size_t revision_ = 0;
};

/// Marks the labels and edges that the run saw. An edge of the trace that no edge of the scripts
/// has (the target was computed) is added as `kTrace`. It also marks the labels that the traced
/// edges reach as reachable. It replaces what an earlier call added, so it can run again with a
/// longer trace.
void apply_trace(StoryGraph& graph, const TraceRecorder& trace);

/// `text` as a CSV field (RFC 4180): in quotes when it has a comma, a quote or a line end.
[[nodiscard]] std::string csv_field(std::string_view text);

/// "jump", "call", "menu" or "trace".
[[nodiscard]] std::string_view edge_kind_name(EdgeKind kind);

/// The graph in Graphviz DOT. Unreachable labels are red, dead ends are filled gray, `ending`
/// labels have a double border, edges and labels that a run saw are green, `menu` edges are blue,
/// `call` edges are dashed and computed targets point to the node "(dynamic)".
[[nodiscard]] std::string to_dot(const StoryGraph& graph);

/// The graph as CSV (RFC 4180, LF line ends) with the header `type,from,to,script,line,flags`. A
/// row for each label (type `label`, name in `from`), then a row for each edge (type `jump`,
/// `call`, `menu` or `trace`). `flags` joins with ";", in this order: for a label `ending`,
/// `unreachable`, `dead_end`, `seen`; for an edge `dynamic`, `seen`. The line is empty when 0.
[[nodiscard]] std::string to_csv(const StoryGraph& graph);

}  // namespace vnmaki
