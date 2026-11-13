#include "engine/tools/story_graph.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/tools/script_scan.hpp"

namespace vnmaki {

namespace {

constexpr std::string_view kDynamicNode = "(dynamic)";
constexpr std::string_view kScriptsDirectory = "scripts";
constexpr std::string_view kLuaExtension = ".lua";
constexpr std::string_view kGreenColor = "color=green4";

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
  std::string joined;
  for (const std::string& part : parts) {
    if (!joined.empty()) {
      joined += separator;
    }
    joined += part;
  }
  return joined;
}

void mark_reachable(StoryGraph& graph) {
  std::map<std::string, std::size_t, std::less<>> index;
  for (std::size_t i = 0; i < graph.labels.size(); ++i) {
    graph.labels[i].reachable = false;
    index.emplace(graph.labels[i].name, i);
  }
  std::vector<std::size_t> pending;
  if (const auto start = index.find(kStartLabel); start != index.end()) {
    graph.labels[start->second].reachable = true;
    pending.push_back(start->second);
  }
  while (!pending.empty()) {
    const std::string from = graph.labels[pending.back()].name;
    pending.pop_back();
    for (const GraphEdge& edge : graph.edges) {
      if (edge.from != from || edge.dynamic) {
        continue;
      }
      const auto target = index.find(edge.to);
      if (target != index.end() && !graph.labels[target->second].reachable) {
        graph.labels[target->second].reachable = true;
        pending.push_back(target->second);
      }
    }
  }
}

std::string dot_quote(std::string_view text) {
  std::string quoted = "\"";
  for (const char c : text) {
    if (c == '"' || c == '\\') {
      quoted.push_back('\\');
    }
    if (c == '\n') {
      quoted += "\\n";
    } else {
      quoted.push_back(c);
    }
  }
  quoted.push_back('"');
  return quoted;
}

std::string csv_field(std::string_view text) {
  if (text.find_first_of(",\"\r\n") == std::string_view::npos) {
    return std::string{text};
  }
  std::string quoted = "\"";
  for (const char c : text) {
    if (c == '"') {
      quoted.push_back('"');
    }
    quoted.push_back(c);
  }
  quoted.push_back('"');
  return quoted;
}

std::string line_text(std::uint32_t line) { return line == 0 ? "" : std::to_string(line); }

}  // namespace

std::vector<ScriptSource> read_scripts(const Vfs& vfs) {
  std::vector<ScriptSource> scripts;
  std::vector<std::string> directories{std::string{kScriptsDirectory}};
  std::vector<std::string> paths;
  while (!directories.empty()) {
    const std::string directory = std::move(directories.back());
    directories.pop_back();
    for (const VfsEntry& entry : vfs.list(directory)) {
      std::string path = directory + "/" + entry.name;
      if (entry.is_directory) {
        directories.push_back(std::move(path));
      } else if (path.ends_with(kLuaExtension)) {
        paths.push_back(std::move(path));
      }
    }
  }
  std::ranges::sort(paths);
  scripts.reserve(paths.size());
  for (std::string& path : paths) {
    const std::optional<std::vector<std::byte>> bytes = vfs.read(path);
    if (!bytes) {
      continue;
    }
    std::string text;
    text.reserve(bytes->size());
    for (const std::byte byte : *bytes) {
      text.push_back(static_cast<char>(byte));
    }
    scripts.push_back({.path = std::move(path), .text = std::move(text)});
  }
  return scripts;
}

StoryGraph build_story_graph(std::span<const ScriptSource> scripts) {
  StoryGraph graph;
  std::set<std::string, std::less<>> defined;
  std::set<std::string, std::less<>> called;
  std::set<std::string, std::less<>> leaves;  // labels with a jump or a ret
  for (const ScriptSource& script : scripts) {
    for (const ScanLabel& scanned : scan_labels(script.text)) {
      if (!scanned.name || !defined.insert(*scanned.name).second) {
        continue;
      }
      graph.labels.push_back({.name = *scanned.name,
                              .script = script.path,
                              .line = scanned.line,
                              .ending = scanned.ending});
      std::optional<std::uint32_t> menu_depth;
      for (const ScanCall& call : scanned.calls) {
        if (call.kind == ScanCallKind::kMenu) {
          menu_depth = call.depth;
        } else if (call.kind == ScanCallKind::kRet) {
          leaves.insert(*scanned.name);
        } else if (call.kind == ScanCallKind::kJump || call.kind == ScanCallKind::kCall) {
          const bool is_jump = call.kind == ScanCallKind::kJump;
          const std::optional<std::string> target =
              call.arguments.empty() ? std::nullopt : call.arguments.front();
          if (is_jump) {
            leaves.insert(*scanned.name);
          } else if (target) {
            called.insert(*target);
          }
          const bool in_menu = menu_depth && call.depth > *menu_depth;
          graph.edges.push_back(
              {.from = *scanned.name,
               .to = target.value_or(""),
               .kind = in_menu ? EdgeKind::kMenu : (is_jump ? EdgeKind::kJump : EdgeKind::kCall),
               .dynamic = !target,
               .script = script.path,
               .line = call.line});
        }
      }
    }
  }
  for (GraphLabel& label : graph.labels) {
    label.dead_end = !label.ending && !leaves.contains(label.name) && !called.contains(label.name);
  }
  mark_reachable(graph);
  return graph;
}

void TraceRecorder::on_label_entered(std::string_view label) {
  const std::string name{label};
  if (std::ranges::find(labels_, name) == labels_.end()) {
    labels_.push_back(name);
    ++revision_;
  }
  if (!current_.empty()) {
    TraceEdge edge{.from = current_, .to = name};
    if (std::ranges::find(edges_, edge) == edges_.end()) {
      edges_.push_back(std::move(edge));
      ++revision_;
    }
  }
  current_ = name;
}

void TraceRecorder::on_label_resumed(std::string_view label) { current_ = std::string{label}; }

void TraceRecorder::clear() {
  edges_.clear();
  labels_.clear();
  current_.clear();
  ++revision_;
}

void apply_trace(StoryGraph& graph, const TraceRecorder& trace) {
  std::erase_if(graph.edges, [](const GraphEdge& edge) { return edge.kind == EdgeKind::kTrace; });
  for (GraphEdge& edge : graph.edges) {
    edge.seen = false;
  }
  for (GraphLabel& label : graph.labels) {
    const std::vector<std::string>& entered = trace.labels();
    label.seen = std::ranges::find(entered, label.name) != entered.end();
  }
  for (const TraceEdge& traced : trace.edges()) {
    bool found = false;
    for (GraphEdge& edge : graph.edges) {
      if (!edge.dynamic && edge.from == traced.from && edge.to == traced.to) {
        edge.seen = true;
        found = true;
      }
    }
    if (!found) {
      graph.edges.push_back(
          {.from = traced.from, .to = traced.to, .kind = EdgeKind::kTrace, .seen = true});
    }
  }
  mark_reachable(graph);
}

std::string_view edge_kind_name(EdgeKind kind) {
  switch (kind) {
    case EdgeKind::kJump:
      return "jump";
    case EdgeKind::kCall:
      return "call";
    case EdgeKind::kMenu:
      return "menu";
    case EdgeKind::kTrace:
      return "trace";
  }
  return "jump";
}

std::string to_dot(const StoryGraph& graph) {
  std::string dot = "digraph story {\n  rankdir=LR;\n  node [shape=box];\n";
  bool any_dynamic = false;
  for (const GraphLabel& label : graph.labels) {
    std::vector<std::string> attributes;
    if (!label.reachable) {
      attributes.emplace_back("color=red");
    } else if (label.seen) {
      attributes.emplace_back(kGreenColor);
    }
    if (label.dead_end) {
      attributes.emplace_back("style=filled");
      attributes.emplace_back("fillcolor=lightgray");
    }
    if (label.ending) {
      attributes.emplace_back("peripheries=2");
    }
    dot += "  " + dot_quote(label.name);
    if (!attributes.empty()) {
      dot += " [" + join(attributes, ", ") + "]";
    }
    dot += ";\n";
  }
  for (const GraphEdge& edge : graph.edges) {
    any_dynamic = any_dynamic || edge.dynamic;
    std::vector<std::string> attributes{"label=" + dot_quote(edge_kind_name(edge.kind))};
    if (edge.seen || edge.kind == EdgeKind::kTrace) {
      attributes.emplace_back(kGreenColor);
    } else if (edge.kind == EdgeKind::kMenu) {
      attributes.emplace_back("color=blue");
    }
    if (edge.dynamic) {
      attributes.emplace_back("style=dotted");
    } else if (edge.kind == EdgeKind::kCall) {
      attributes.emplace_back("style=dashed");
    }
    dot += "  " + dot_quote(edge.from) + " -> " +
           dot_quote(edge.dynamic ? kDynamicNode : std::string_view{edge.to}) + " [" +
           join(attributes, ", ") + "];\n";
  }
  if (any_dynamic) {
    dot += "  " + dot_quote(kDynamicNode) + " [shape=plaintext];\n";
  }
  dot += "}\n";
  return dot;
}

std::string to_csv(const StoryGraph& graph) {
  std::string csv = "type,from,to,script,line,flags\n";
  for (const GraphLabel& label : graph.labels) {
    std::vector<std::string> flags;
    if (label.ending) {
      flags.emplace_back("ending");
    }
    if (!label.reachable) {
      flags.emplace_back("unreachable");
    }
    if (label.dead_end) {
      flags.emplace_back("dead_end");
    }
    if (label.seen) {
      flags.emplace_back("seen");
    }
    csv += "label," + csv_field(label.name) + ",," + csv_field(label.script) + "," +
           line_text(label.line) + "," + join(flags, ";") + "\n";
  }
  for (const GraphEdge& edge : graph.edges) {
    std::vector<std::string> flags;
    if (edge.dynamic) {
      flags.emplace_back("dynamic");
    }
    if (edge.seen) {
      flags.emplace_back("seen");
    }
    csv += std::string{edge_kind_name(edge.kind)} + "," + csv_field(edge.from) + "," +
           csv_field(edge.to) + "," + csv_field(edge.script) + "," + line_text(edge.line) + "," +
           join(flags, ";") + "\n";
  }
  return csv;
}

}  // namespace vnmaki
