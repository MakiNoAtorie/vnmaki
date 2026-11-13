#include "engine/tools/overlay.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <imgui.h>

#include "engine/audio/audio.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/imgui_backend.hpp"
#include "engine/tools/inspect.hpp"
#include "engine/tools/profiler.hpp"
#include "engine/tools/repl.hpp"

namespace vnmaki {

namespace {

constexpr float kConsoleX = 16;
constexpr float kConsoleY = 16;
constexpr float kConsoleWidth = 640;
constexpr float kConsoleHeight = 360;
// Packed ABGR, the format ImGui's style colors take.
constexpr ImU32 kWarningColor = 0xFF33CCFF;
constexpr ImU32 kErrorColor = 0xFF6666FF;
constexpr float kInspectorX = 16;
constexpr float kInspectorY = 392;  // under the console
constexpr float kInspectorWidth = 640;
constexpr float kInspectorHeight = 300;
constexpr float kEditSpeed = 0.01F;
// The second color of the story graph: what the run took.
constexpr ImU32 kSeenColor = 0xFF66CC66;
constexpr float kGraphX = 680;
constexpr float kGraphY = 16;
constexpr float kGraphWidth = 560;
constexpr float kGraphHeight = 420;
constexpr float kLabelsTableHeight = 150;
constexpr float kProfilerX = 680;
constexpr float kProfilerY = 452;  // under the story graph
constexpr float kProfilerWidth = 560;
constexpr float kProfilerHeight = 270;
constexpr float kPlotHeight = 70;
constexpr std::size_t kSlowestLoads = 8;
constexpr double kMillisecondsPerSecond = 1000.0;
constexpr double kBytesPerMebibyte = 1024.0 * 1024.0;

double to_ms(Duration time) {
  return std::chrono::duration<double>(time).count() * kMillisecondsPerSecond;
}

// The ImGui key of an engine key, or ImGuiKey_None for one it has no use for.
ImGuiKey to_imgui_key(Key key) {
  const auto in_run = [key](Key first, Key last, ImGuiKey start) -> ImGuiKey {
    if (key < first || key > last) {
      return ImGuiKey_None;
    }
    return static_cast<ImGuiKey>(static_cast<int>(start) +
                                 (static_cast<int>(key) - static_cast<int>(first)));
  };
  for (const ImGuiKey run :
       {in_run(Key::kA, Key::kZ, ImGuiKey_A), in_run(Key::kDigit0, Key::kDigit9, ImGuiKey_0),
        in_run(Key::kF1, Key::kF12, ImGuiKey_F1)}) {
    if (run != ImGuiKey_None) {
      return run;
    }
  }
  switch (key) {
    case Key::kEscape:
      return ImGuiKey_Escape;
    case Key::kEnter:
      return ImGuiKey_Enter;
    case Key::kKeypadEnter:
      return ImGuiKey_KeypadEnter;
    case Key::kSpace:
      return ImGuiKey_Space;
    case Key::kTab:
      return ImGuiKey_Tab;
    case Key::kBackspace:
      return ImGuiKey_Backspace;
    case Key::kDelete:
      return ImGuiKey_Delete;
    case Key::kInsert:
      return ImGuiKey_Insert;
    case Key::kHome:
      return ImGuiKey_Home;
    case Key::kEnd:
      return ImGuiKey_End;
    case Key::kPageUp:
      return ImGuiKey_PageUp;
    case Key::kPageDown:
      return ImGuiKey_PageDown;
    case Key::kLeft:
      return ImGuiKey_LeftArrow;
    case Key::kRight:
      return ImGuiKey_RightArrow;
    case Key::kUp:
      return ImGuiKey_UpArrow;
    case Key::kDown:
      return ImGuiKey_DownArrow;
    case Key::kLeftShift:
      return ImGuiKey_LeftShift;
    case Key::kRightShift:
      return ImGuiKey_RightShift;
    case Key::kLeftCtrl:
      return ImGuiKey_LeftCtrl;
    case Key::kRightCtrl:
      return ImGuiKey_RightCtrl;
    case Key::kLeftAlt:
      return ImGuiKey_LeftAlt;
    case Key::kRightAlt:
      return ImGuiKey_RightAlt;
    case Key::kLeftGui:
      return ImGuiKey_LeftSuper;
    case Key::kRightGui:
      return ImGuiKey_RightSuper;
    default:
      return ImGuiKey_None;
  }
}

// ImGui's button index: left 0, right 1, middle 2, then the side buttons.
int to_imgui_button(MouseButton button) {
  switch (button) {
    case MouseButton::kLeft:
      return ImGuiMouseButton_Left;
    case MouseButton::kRight:
      return ImGuiMouseButton_Right;
    case MouseButton::kMiddle:
      return ImGuiMouseButton_Middle;
    case MouseButton::kX1:
      return 3;
    case MouseButton::kX2:
      return 4;
  }
  return ImGuiMouseButton_Left;
}

std::string label_state(const GraphLabel& label) {
  std::string state;
  const auto add = [&state](const char* word) {
    state += state.empty() ? "" : ", ";
    state += word;
  };
  if (!label.reachable) {
    add("unreachable");
  }
  if (label.dead_end) {
    add("dead end");
  }
  if (label.ending) {
    add("ending");
  }
  if (label.seen) {
    add("seen");
  }
  return state;
}

// Text in `color`, or in the style's color when `color` is 0.
void colored_cell(const std::string& text, ImU32 color) {
  if (color != 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, color);
  }
  ImGui::TextUnformatted(text.c_str());
  if (color != 0) {
    ImGui::PopStyleColor();
  }
}

}  // namespace

DevOverlay::DevOverlay(SdlGpuRenderer& renderer, Platform& platform, ScriptRuntime& runtime,
                       LogBuffer& log)
    : backend_(std::make_unique<ImguiBackend>(renderer)),
      platform_(&platform),
      runtime_(&runtime),
      log_(&log) {}

DevOverlay::~DevOverlay() {
  if (text_input_) {
    platform_->set_text_input(false);
  }
}

void DevOverlay::set_story(const Vfs& vfs, const TraceRecorder& trace) {
  vfs_ = &vfs;
  trace_ = &trace;
  graph_ = build_story_graph(read_scripts(vfs));
  trace_revision_ = trace.revision();
  apply_trace(graph_, trace);
}

void DevOverlay::set_visible(bool visible) {
  visible_ = visible;
  focus_input_ = visible;
  if (!visible && text_input_) {
    platform_->set_text_input(false);
    text_input_ = false;
  }
}

bool DevOverlay::handle_event(const InputEvent& event) {
  if (const auto* key = std::get_if<KeyEvent>(&event); key != nullptr && key->key == Key::kF12) {
    if (key->pressed && !key->repeat) {
      set_visible(!visible_);
    }
    return true;
  }
  if (!visible_) {
    return false;
  }
  ImGuiIO& io = ImGui::GetIO();
  if (const auto* key = std::get_if<KeyEvent>(&event)) {
    const ImGuiKey imgui_key = to_imgui_key(key->key);
    if (imgui_key != ImGuiKey_None) {
      io.AddKeyEvent(imgui_key, key->pressed);
    }
    return io.WantCaptureKeyboard;
  }
  if (const auto* text = std::get_if<TextInputEvent>(&event)) {
    io.AddInputCharactersUTF8(text->text.c_str());
    return io.WantTextInput;
  }
  if (const auto* move = std::get_if<PointerMoveEvent>(&event)) {
    io.AddMousePosEvent(move->position.x, move->position.y);
    return io.WantCaptureMouse;
  }
  if (const auto* button = std::get_if<PointerButtonEvent>(&event)) {
    io.AddMousePosEvent(button->position.x, button->position.y);
    io.AddMouseButtonEvent(to_imgui_button(button->button), button->pressed);
    return io.WantCaptureMouse;
  }
  if (const auto* wheel = std::get_if<WheelEvent>(&event)) {
    io.AddMouseWheelEvent(-wheel->dx, wheel->dy);  // ImGui: a negative x scrolls right
    return io.WantCaptureMouse;
  }
  return false;
}

void DevOverlay::draw(Size window_pixels) {
  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - last_frame_).count();
  last_frame_ = now;
  if (!visible_) {
    return;
  }
  backend_->begin_frame(window_pixels, seconds);
  draw_console();
  draw_inspectors();
  if (vfs_ != nullptr) {
    draw_story_graph();
  }
  if (profiler_ != nullptr) {
    draw_profiler();
  }
  const bool wants_text = ImGui::GetIO().WantTextInput;
  if (wants_text != text_input_) {
    platform_->set_text_input(wants_text);
    text_input_ = wants_text;
  }
  backend_->end_frame();
}

void DevOverlay::set_inspect_targets(Scene* scene, Audio* audio) {
  scene_ = scene;
  audio_ = audio;
}

void DevOverlay::run_input() {
  const std::string line{input_.data()};
  input_.fill('\0');
  if (!line.empty()) {
    run_repl_line(*runtime_, *log_, line);
  }
  focus_input_ = true;
}

void DevOverlay::draw_console() {
  constexpr std::array<const char*, kLevels> level_names{"Info", "Warning", "Error"};
  ImGui::SetNextWindowPos(ImVec2{kConsoleX, kConsoleY}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2{kConsoleWidth, kConsoleHeight}, ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Console")) {
    for (std::size_t level = 0; level < kLevels; ++level) {
      ImGui::Checkbox(level_names.at(level), &show_level_.at(level));
      ImGui::SameLine();
    }
    if (ImGui::Button("Clear")) {
      log_->clear();
    }
    ImGui::Separator();
    if (ImGui::BeginChild("log", ImVec2{0, -ImGui::GetFrameHeightWithSpacing()},
                          ImGuiChildFlags_Borders)) {
      ImGui::PushTextWrapPos(0);
      for (const LogEntry& entry : log_->entries()) {
        if (!show_level_.at(static_cast<std::size_t>(entry.level))) {
          continue;
        }
        const bool plain = entry.level == LogLevel::kInfo;
        if (!plain) {
          ImGui::PushStyleColor(ImGuiCol_Text,
                                entry.level == LogLevel::kError ? kErrorColor : kWarningColor);
        }
        ImGui::TextUnformatted(entry.text.c_str());
        if (!plain) {
          ImGui::PopStyleColor();
        }
      }
      ImGui::PopTextWrapPos();
      if (log_->total() != shown_total_) {
        shown_total_ = log_->total();
        ImGui::SetScrollHereY(1);
      }
    }
    ImGui::EndChild();
    if (focus_input_) {
      ImGui::SetKeyboardFocusHere();
      focus_input_ = false;
    }
    ImGui::SetNextItemWidth(-std::numeric_limits<float>::min());
    if (ImGui::InputText("##lua", input_.data(), input_.size(),
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
      run_input();
    }
  }
  ImGui::End();
}

// --- Inspectors: ImGui only draws the rows of engine/tools/inspect.hpp ---------------------------

void DevOverlay::draw_inspectors() {
  ImGui::SetNextWindowPos(ImVec2{kInspectorX, kInspectorY}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2{kInspectorWidth, kInspectorHeight}, ImGuiCond_FirstUseEver);
  // The console keeps the keyboard focus that it asks for when the overlay opens.
  const bool open = ImGui::Begin("Inspectors", nullptr, ImGuiWindowFlags_NoFocusOnAppearing);
  if (open && ImGui::BeginTabBar("inspector_tabs")) {
    if (ImGui::BeginTabItem("Store")) {
      draw_store_tree(inspect_store(*runtime_));
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Persistent")) {
      draw_store_tree(inspect_persistent(*runtime_));
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Scene")) {
      draw_scene_tab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Tweens")) {
      draw_tweens_tab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Audio")) {
      draw_audio_tab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Labels")) {
      draw_labels_tab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

void DevOverlay::draw_story_graph() {
  if (trace_->revision() != trace_revision_) {
    trace_revision_ = trace_->revision();
    apply_trace(graph_, *trace_);
  }
  ImGui::SetNextWindowPos(ImVec2{kGraphX, kGraphY}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2{kGraphWidth, kGraphHeight}, ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Story graph")) {
    ImGui::TextUnformatted((std::to_string(graph_.labels.size()) + " labels, " +
                            std::to_string(graph_.edges.size()) + " edges")
                               .c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kSeenColor);
    ImGui::TextUnformatted("green: taken in this run");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("Rescan")) {
      graph_ = build_story_graph(read_scripts(*vfs_));
      apply_trace(graph_, *trace_);
    }
    constexpr ImGuiTableFlags kTableFlags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY;
    if (ImGui::BeginTable("labels", 3, kTableFlags, ImVec2{0, kLabelsTableHeight})) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Label");
      ImGui::TableSetupColumn("Source");
      ImGui::TableSetupColumn("State");
      ImGui::TableHeadersRow();
      for (const GraphLabel& label : graph_.labels) {
        ImU32 color = label.seen ? kSeenColor : 0;
        color = label.dead_end ? kWarningColor : color;
        color = label.reachable ? color : kErrorColor;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        colored_cell(label.name, color);
        ImGui::TableNextColumn();
        colored_cell(label.script + ":" + std::to_string(label.line), 0);
        ImGui::TableNextColumn();
        colored_cell(label_state(label), color);
      }
      ImGui::EndTable();
    }
    if (ImGui::BeginTable("edges", 4, kTableFlags)) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("From");
      ImGui::TableSetupColumn("To");
      ImGui::TableSetupColumn("Kind");
      ImGui::TableSetupColumn("Line");
      ImGui::TableHeadersRow();
      for (const GraphEdge& edge : graph_.edges) {
        const ImU32 color = edge.seen ? kSeenColor : 0;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        colored_cell(edge.from, color);
        ImGui::TableNextColumn();
        colored_cell(edge.dynamic ? "(computed)" : edge.to, color);
        ImGui::TableNextColumn();
        colored_cell(std::string{edge_kind_name(edge.kind)}, color);
        ImGui::TableNextColumn();
        colored_cell(edge.line == 0 ? "" : std::to_string(edge.line), 0);
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

// The rows come depth first, so one TreeNode is open for each level above the row. `open` is how
// many are; a closed table hides the rows below it.
void DevOverlay::draw_profiler() {
  ImGui::SetNextWindowPos(ImVec2{kProfilerX, kProfilerY}, ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2{kProfilerWidth, kProfilerHeight}, ImGuiCond_FirstUseEver);
  if (ImGui::Begin("Profiler")) {
    const std::deque<FrameSample>& frames = profiler_->frames();
    std::vector<float> totals;
    totals.reserve(frames.size());
    for (const FrameSample& frame : frames) {
      totals.push_back(static_cast<float>(to_ms(frame.total())));
    }
    ImGui::PlotLines("##frame_ms", totals.data(), static_cast<int>(totals.size()), 0,
                     "frame ms (CPU, last 300)", 0, std::numeric_limits<float>::max(),
                     ImVec2{0, kPlotHeight});
    if (!frames.empty()) {
      const FrameSample& last = frames.back();
      ImGui::Text("events %.2f  update %.2f (x%d)  prepare %.2f  draw %.2f  total %.2f ms",
                  to_ms(last.events), to_ms(last.update), last.updates, to_ms(last.prepare),
                  to_ms(last.draw), to_ms(last.total()));
      ImGui::Text("passes %u  quads %u  draw calls %u", last.gpu.passes, last.gpu.quads,
                  last.gpu.draw_calls);
      ImGui::Text("textures %u: %.2f MiB   render targets %u: %.2f MiB", last.memory.textures,
                  static_cast<double>(last.memory.texture_bytes) / kBytesPerMebibyte,
                  last.memory.targets,
                  static_cast<double>(last.memory.target_bytes) / kBytesPerMebibyte);
    }
    ImGui::Separator();
    ImGui::TextUnformatted("Slowest loads");
    constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders;
    if (ImGui::BeginTable("loads", 2, kTableFlags)) {
      ImGui::TableSetupColumn("ms", ImGuiTableColumnFlags_WidthFixed);
      ImGui::TableSetupColumn("path");
      ImGui::TableHeadersRow();
      for (const LoadSample& load : profiler_->slowest_loads(kSlowestLoads)) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", to_ms(load.time));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(load.path.c_str());
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

void DevOverlay::draw_store_tree(const std::vector<StoreRow>& rows) {
  int open = 0;
  int hidden_below = -1;  // the depth of a closed table: deeper rows are skipped
  for (const StoreRow& row : rows) {
    while (open > row.depth) {
      ImGui::TreePop();
      --open;
    }
    if (hidden_below >= 0 && row.depth > hidden_below) {
      continue;
    }
    hidden_below = -1;
    if (row.is_table && row.value.empty()) {
      if (ImGui::TreeNode(row.key.c_str())) {
        ++open;
      } else {
        hidden_below = row.depth;
      }
    } else {
      ImGui::Text("%s = %s", row.key.c_str(), row.value.c_str());
    }
  }
  while (open > 0) {
    ImGui::TreePop();
    --open;
  }
}

void DevOverlay::draw_scene_tab() {
  if (scene_ == nullptr) {
    ImGui::TextUnformatted("No scene.");
    return;
  }
  for (const LayerRows& layer : inspect_scene(*scene_)) {
    if (!ImGui::TreeNodeEx(layer.layer.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
      continue;
    }
    for (const DisplayableRow& displayable : layer.displayables) {
      const std::string label = displayable.tag + "  (" + displayable.kind + ")";
      if (ImGui::Selectable(label.c_str(), displayable.tag == selected_tag_)) {
        selected_tag_ = displayable.tag;
      }
    }
    ImGui::TreePop();
  }
  ImGui::Separator();
  const std::vector<PropertyRow> properties = inspect_properties(*scene_, selected_tag_);
  if (properties.empty()) {
    ImGui::TextUnformatted("Select a displayable to see its properties.");
    return;
  }
  ImGui::Text("Properties of '%s'", selected_tag_.c_str());
  ImGui::TextWrapped(
      "An edit applies at once. It is not in the snapshot of the label: a rollback or a load "
      "brings back the old value.");
  for (const PropertyRow& property : properties) {
    auto value = static_cast<float>(property.value);
    // A drag changes the value by 1% a pixel, so both pixel values and fractions are easy to set.
    const float speed = std::max(kEditSpeed, std::abs(value) * kEditSpeed);
    if (ImGui::DragFloat(property.name.c_str(), &value, speed)) {
      static_cast<void>(set_property(*scene_, selected_tag_, property.name, value));
    }
  }
}

void DevOverlay::draw_tweens_tab() {
  if (scene_ == nullptr) {
    ImGui::TextUnformatted("No scene.");
    return;
  }
  const std::vector<TweenRow> tweens = inspect_tweens(*scene_);
  if (tweens.empty()) {
    ImGui::TextUnformatted("No tween is running.");
  }
  for (const TweenRow& tween : tweens) {
    ImGui::Text("%s: %s  %.2f / %.2f s  %s%s", tween.tag.c_str(), tween.channels.c_str(),
                tween.elapsed, tween.duration, tween.ease.c_str(), tween.loop ? "  loop" : "");
  }
}

void DevOverlay::draw_audio_tab() {
  if (audio_ == nullptr) {
    ImGui::TextUnformatted("No audio.");
    return;
  }
  for (const AudioRow& row : inspect_audio(*audio_)) {
    if (row.file.empty()) {
      ImGui::Text("%s: (silent)  volume %.2f", row.channel.c_str(), row.volume);
    } else {
      ImGui::Text("%s: %s  volume %.2f  at %.2f s%s", row.channel.c_str(), row.file.c_str(),
                  row.volume, row.position, row.loop ? "  loop" : "");
    }
  }
}

void DevOverlay::draw_labels_tab() {
  for (const LabelRow& label : inspect_labels(*runtime_)) {
    ImGui::PushID(label.name.c_str());
    if (ImGui::Button("Jump")) {
      const std::string problem = jump_to_label(*runtime_, label.name);
      if (problem.empty()) {
        log_->add(LogLevel::kInfo, "jumped to " + label.name);
      } else {
        log_->add(LogLevel::kError, problem);
      }
    }
    ImGui::SameLine();
    ImGui::Text("%s%s", label.name.c_str(), label.current ? "  (current)" : "");
    ImGui::PopID();
  }
}

}  // namespace vnmaki
