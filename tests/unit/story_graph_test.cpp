// The story graph (engine/tools/story_graph.hpp): edges, unreachable labels and dead ends from the
// scripts, the traces of a run, and the DOT and CSV text. Expected values are written out by hand
// from the script text. The generated fixture project is checked through the CLI
// (tests/cli/CMakeLists.txt).

#include "engine/tools/story_graph.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <doctest/doctest.h>

#include "engine/runtime/script_runtime.hpp"
#include "engine/tools/observer_chain.hpp"
#include "engine/vfs/vfs.hpp"
#include "tests/support/memory_vfs.hpp"

namespace {

using vnmaki::EdgeKind;
using vnmaki::GraphEdge;
using vnmaki::GraphLabel;
using vnmaki::RunState;
using vnmaki::ScriptSource;
using vnmaki::StoryGraph;

constexpr std::size_t kStepLimit = 100;

const GraphLabel& label_of(const StoryGraph& graph, std::string_view name) {
  for (const GraphLabel& label : graph.labels) {
    if (label.name == name) {
      return label;
    }
  }
  FAIL("no label " << name);
  return graph.labels.front();
}

std::vector<std::string> names_where(const StoryGraph& graph, bool GraphLabel::* flag,
                                     bool wanted) {
  std::vector<std::string> names;
  for (const GraphLabel& label : graph.labels) {
    if (label.*flag == wanted) {
      names.push_back(label.name);
    }
  }
  return names;
}

using Names = std::vector<std::string>;

// Two scripts: a menu with a jump and a call in its branches, a computed jump, a label that ends
// on purpose, one that ends by accident, one that nobody reaches and a second `start`.
std::vector<ScriptSource> sample_scripts() {
  return {{.path = "scripts/a.lua",
           .text = "label(\"start\", function()\n"  // 1
                   "  menu { \"x\", \"y\" }\n"      // 2
                   "  if c == 1 then\n"             // 3
                   "    jump(\"shop\")\n"           // 4
                   "  else\n"                       // 5
                   "    call(\"sub\")\n"            // 6
                   "  end\n"                        // 7
                   "  jump(\"end_ok\")\n"           // 8
                   "end)\n"                         // 9
                   "\n"                             // 10
                   "label(\"sub\", function()\n"    // 11
                   "  say(\"A\", \"hi\")\n"         // 12
                   "end)\n"},                       // 13
          {.path = "scripts/b.lua",
           .text = "label(\"shop\", function()\n"                                             // 1
                   "  jump(where)\n"                                                          // 2
                   "end)\n"                                                                   // 3
                   "label(\"end_ok\", function() narrate(\"bye\") end, { ending = true })\n"  // 4
                   "label(\"stuck\", function() narrate(\"oops\") end)\n"                     // 5
                   "label(\"orphan\", function() ret() end)\n"                                // 6
                   "label(\"start\", function() jump(\"stuck\") end)\n"}};                    // 7
}

}  // namespace

TEST_CASE("story_graph: labels and edges from the scripts") {
  const StoryGraph graph = vnmaki::build_story_graph(sample_scripts());
  CHECK(names_where(graph, &GraphLabel::reachable, true) ==
        Names{"start", "sub", "shop", "end_ok"});
  CHECK(label_of(graph, "start").script == "scripts/a.lua");
  CHECK(label_of(graph, "start").line == 1);  // the second `start` does not count
  CHECK(label_of(graph, "end_ok").ending);

  const std::vector<GraphEdge> expected{
      {.from = "start",
       .to = "shop",
       .kind = EdgeKind::kMenu,
       .script = "scripts/a.lua",
       .line = 4},
      {.from = "start", .to = "sub", .kind = EdgeKind::kMenu, .script = "scripts/a.lua", .line = 6},
      {.from = "start",
       .to = "end_ok",
       .kind = EdgeKind::kJump,
       .script = "scripts/a.lua",
       .line = 8},
      {.from = "shop",
       .kind = EdgeKind::kJump,
       .dynamic = true,
       .script = "scripts/b.lua",
       .line = 2}};
  CHECK(graph.edges == expected);
}

TEST_CASE("story_graph: unreachable labels and dead ends") {
  const StoryGraph graph = vnmaki::build_story_graph(sample_scripts());
  CHECK(names_where(graph, &GraphLabel::reachable, false) == Names{"stuck", "orphan"});
  // `sub` is called and runs to its end, `shop` jumps (to a computed label), `end_ok` is marked,
  // `orphan` returns.
  CHECK(names_where(graph, &GraphLabel::dead_end, true) == Names{"stuck"});
}

TEST_CASE("story_graph: a story with no start label reaches nothing") {
  const StoryGraph graph = vnmaki::build_story_graph(std::vector<ScriptSource>{
      {.path = "scripts/a.lua",
       .text = "label(\"a\", function() jump(\"b\") end)\nlabel(\"b\", function() end)"}});
  CHECK(names_where(graph, &GraphLabel::reachable, false) == Names{"a", "b"});
}

TEST_CASE("story_graph: CSV text") {
  const StoryGraph graph = vnmaki::build_story_graph(sample_scripts());
  CHECK(vnmaki::to_csv(graph) ==
        "type,from,to,script,line,flags\n"
        "label,start,,scripts/a.lua,1,\n"
        "label,sub,,scripts/a.lua,11,\n"
        "label,shop,,scripts/b.lua,1,\n"
        "label,end_ok,,scripts/b.lua,4,ending\n"
        "label,stuck,,scripts/b.lua,5,unreachable;dead_end\n"
        "label,orphan,,scripts/b.lua,6,unreachable\n"
        "menu,start,shop,scripts/a.lua,4,\n"
        "menu,start,sub,scripts/a.lua,6,\n"
        "jump,start,end_ok,scripts/a.lua,8,\n"
        "jump,shop,,scripts/b.lua,2,dynamic\n");
}

TEST_CASE("story_graph: CSV quotes a field with a comma or a quote") {
  const StoryGraph graph = vnmaki::build_story_graph(std::vector<ScriptSource>{
      {.path = "scripts/a,b.lua",
       .text = R"(label("start", function() jump("say \"hi\", ok") end))"}});
  CHECK(vnmaki::to_csv(graph) ==
        "type,from,to,script,line,flags\n"
        "label,start,,\"scripts/a,b.lua\",1,\n"
        "jump,start,\"say \"\"hi\"\", ok\",\"scripts/a,b.lua\",1,\n");
}

TEST_CASE("story_graph: DOT text") {
  const StoryGraph graph = vnmaki::build_story_graph(std::vector<ScriptSource>{
      {.path = "scripts/a.lua",
       .text =
           "label(\"start\", function() menu {\"a\"} if c then call(\"x\") end jump(\"y\") end)\n"
           "label(\"x\", function() end)\n"
           "label(\"y\", function() jump(z) end, { ending = true })\n"
           "label(\"lost\", function() end)\n"}});
  CHECK(vnmaki::to_dot(graph) ==
        "digraph story {\n"
        "  rankdir=LR;\n"
        "  node [shape=box];\n"
        "  \"start\";\n"
        "  \"x\";\n"
        "  \"y\" [peripheries=2];\n"
        "  \"lost\" [color=red, style=filled, fillcolor=lightgray];\n"
        "  \"start\" -> \"x\" [label=\"menu\", color=blue];\n"
        "  \"start\" -> \"y\" [label=\"jump\"];\n"
        "  \"y\" -> \"(dynamic)\" [label=\"jump\", style=dotted];\n"
        "  \"(dynamic)\" [shape=plaintext];\n"
        "}\n");
}

namespace {

// Plays the story: answers the first menu option and every call_screen with "start".
void play(vnmaki::ScriptRuntime& runtime) {
  runtime.start("start");
  for (std::size_t step = 0; step < kStepLimit && runtime.state() == RunState::kWaiting; ++step) {
    const vnmaki::BlockingRequest& request = *runtime.request();
    if (std::holds_alternative<vnmaki::MenuRequest>(request)) {
      runtime.choose(0);
    } else if (std::holds_alternative<vnmaki::CallScreenRequest>(request)) {
      runtime.return_screen(std::string{"start"});
    } else {
      runtime.advance();
    }
  }
  REQUIRE(runtime.state() == RunState::kFinished);
}

}  // namespace

TEST_CASE("story_graph: a run marks its edges and adds the one that a computed jump took") {
  const std::string script =
      "store.target = \"hidden\"\n"                             // 1
      "label(\"start\", function()\n"                           // 2
      "  narrate(\"one\")\n"                                    // 3
      "  call(\"helper\")\n"                                    // 4
      "  jump(store.target)\n"                                  // 5
      "end)\n"                                                  // 6
      "label(\"helper\", function() narrate(\"two\") end)\n"    // 7
      "label(\"hidden\", function() narrate(\"three\") end)\n"  // 8
      "label(\"late\", function() jump(\"never\") end)\n"       // 9
      "label(\"never\", function() end)\n";                     // 10
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs({{"scripts/main.lua", script}});
  StoryGraph graph = vnmaki::build_story_graph(vnmaki::read_scripts(vfs));
  CHECK(names_where(graph, &GraphLabel::reachable, false) == Names{"hidden", "late", "never"});

  vnmaki::TraceRecorder trace;
  vnmaki::ScriptRuntime runtime{vfs, {.observer = &trace}};
  runtime.load_scripts();
  play(runtime);
  CHECK(trace.labels() == Names{"start", "helper", "hidden"});
  CHECK(trace.edges() == std::vector<vnmaki::TraceEdge>{{"start", "helper"}, {"start", "hidden"}});

  vnmaki::apply_trace(graph, trace);
  const std::vector<GraphEdge> expected{
      {.from = "start",
       .to = "helper",
       .kind = EdgeKind::kCall,
       .script = "scripts/main.lua",
       .line = 4,
       .seen = true},
      {.from = "start",
       .kind = EdgeKind::kJump,
       .dynamic = true,
       .script = "scripts/main.lua",
       .line = 5},
      {.from = "late",
       .to = "never",
       .kind = EdgeKind::kJump,
       .script = "scripts/main.lua",
       .line = 9},
      {.from = "start", .to = "hidden", .kind = EdgeKind::kTrace, .seen = true}};
  CHECK(graph.edges == expected);
  CHECK(names_where(graph, &GraphLabel::seen, true) == Names{"start", "helper", "hidden"});
  CHECK(names_where(graph, &GraphLabel::reachable, false) == Names{"late", "never"});

  // A second call starts from the scripts again, so it can follow a longer trace.
  vnmaki::apply_trace(graph, trace);
  CHECK(graph.edges == expected);
  CHECK(vnmaki::to_csv(graph).find("trace,start,hidden,,,seen\n") != std::string::npos);
  CHECK(vnmaki::to_dot(graph).find("\"start\" -> \"hidden\" [label=\"trace\", color=green4];") !=
        std::string::npos);
}

TEST_CASE("story_graph: break_trace and clear forget the label that ran") {
  vnmaki::TraceRecorder trace;
  trace.on_label_entered("a");
  trace.break_trace();
  trace.on_label_entered("b");
  CHECK(trace.edges().empty());
  trace.on_label_entered("c");
  trace.on_label_resumed("b");
  trace.on_label_entered("c");
  CHECK(trace.edges() == std::vector<vnmaki::TraceEdge>{{"b", "c"}});
  trace.clear();
  CHECK(trace.labels().empty());
  CHECK(trace.edges().empty());
}

namespace {

const GraphEdge* find_edge(const StoryGraph& graph, std::string_view from, std::string_view to) {
  for (const GraphEdge& edge : graph.edges) {
    if (edge.from == from && edge.to == to) {
      return &edge;
    }
  }
  return nullptr;
}

}  // namespace

TEST_CASE("story_graph template: every label of the sample game is reachable") {
  const std::filesystem::path directory = std::filesystem::path{VNMAKI_TEMPLATES_DIR} / "default";
  vnmaki::Vfs vfs;
  REQUIRE(vfs.mount(vnmaki::make_directory_mount(directory)));
  StoryGraph graph = vnmaki::build_story_graph(vnmaki::read_scripts(vfs));
  CHECK(names_where(graph, &GraphLabel::reachable, false).empty());
  // Not the lines: the template is a sample that people change.
  const GraphEdge* opening = find_edge(graph, "start", "classroom");
  const GraphEdge* warm = find_edge(graph, "classroom", "warm");
  const GraphEdge* cold = find_edge(graph, "classroom", "cold");
  REQUIRE(opening != nullptr);
  REQUIRE(warm != nullptr);
  REQUIRE(cold != nullptr);
  CHECK(opening->kind == EdgeKind::kJump);
  CHECK(warm->kind == EdgeKind::kMenu);
  CHECK(cold->kind == EdgeKind::kMenu);

  // The first menu option takes the path through `warm`.
  vnmaki::TraceRecorder trace;
  vnmaki::ScriptRuntime runtime{vfs, {.observer = &trace}};
  runtime.load_scripts();
  play(runtime);
  vnmaki::apply_trace(graph, trace);
  CHECK(label_of(graph, "warm").seen);
  CHECK_FALSE(label_of(graph, "cold").seen);
  CHECK(find_edge(graph, "start", "classroom")->seen);
  CHECK(find_edge(graph, "classroom", "warm")->seen);
  CHECK_FALSE(find_edge(graph, "classroom", "cold")->seen);
  CHECK(names_where(graph, &GraphLabel::reachable, false).empty());
}

namespace {

class PrintCounter final : public vnmaki::ScriptObserver {
 public:
  void on_print(std::string_view /*text*/) override { ++prints; }

  std::size_t prints = 0;
};

}  // namespace

TEST_CASE("story_graph: an observer chain gives every call to every observer") {
  const vnmaki::Vfs vfs = vnmaki::test::memory_vfs(
      {{"scripts/main.lua",
        "label('start', function() print('x') call('a') end)\nlabel('a', function() end)"}});
  vnmaki::TraceRecorder trace;
  PrintCounter prints;
  vnmaki::ObserverChain chain{&prints, &trace};
  vnmaki::ScriptRuntime runtime{vfs, {.observer = &chain}};
  runtime.load_scripts();
  runtime.start("start");
  CHECK(runtime.state() == RunState::kFinished);
  CHECK(prints.prints == 1);
  CHECK(trace.edges() == std::vector<vnmaki::TraceEdge>{{"start", "a"}});
  const std::size_t revision = trace.revision();
  trace.clear();
  CHECK(trace.revision() != revision);
}
