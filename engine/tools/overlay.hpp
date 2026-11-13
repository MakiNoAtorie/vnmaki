#pragma once

// The dev overlay (docs/SPEC.md section 18): ImGui windows over the game, in dev builds only.

#include <array>
#include <chrono>
#include <cstddef>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine/platform/geometry.hpp"
#include "engine/platform/input.hpp"
#include "engine/platform/platform.hpp"
#include "engine/platform/viewport.hpp"
#include "engine/render/sdl_gpu_renderer.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/tools/story_graph.hpp"
#include "engine/vfs/vfs.hpp"

namespace vnmaki {

class Audio;
class ImguiBackend;
class Profiler;
class Scene;
class TranslationEditor;
class ScreenEditor;
struct StoreRow;

/// The overlay: a log console with a Lua REPL, the story graph, and the inspectors (store,
/// persistent, scene, tweens, audio, labels; their rows come from engine/tools/inspect.hpp). F12
/// shows and hides it.
/// Construct it after the renderer and destroy it before; `platform`, `runtime` and `log` must
/// outlive it.
///
/// The game keeps running under the overlay. While the pointer is over an overlay window, or a
/// text field of the overlay has the keyboard, handle_event() takes the event, so the game gets
/// no input.
class DevOverlay {
 public:
  DevOverlay(SdlGpuRenderer& renderer, Platform& platform, ScriptRuntime& runtime, LogBuffer& log);
  ~DevOverlay();
  DevOverlay(const DevOverlay&) = delete;
  DevOverlay& operator=(const DevOverlay&) = delete;
  DevOverlay(DevOverlay&&) = delete;
  DevOverlay& operator=(DevOverlay&&) = delete;

  [[nodiscard]] bool visible() const { return visible_; }
  void set_visible(bool visible);

  /// Feeds the overlay an event. Returns true when the overlay took it and the game must not
  /// see it: F12 always, and, while the overlay is visible, the pointer and keyboard events it
  /// wants.
  [[nodiscard]] bool handle_event(const InputEvent& event);

  /// The scene and audio that the inspector panels show. Both may be null (their tabs say so);
  /// they must outlive the overlay or be reset first.
  void set_inspect_targets(Scene* scene, Audio* audio);

  /// Turns on the "Story graph" window: the labels and edges of the scripts in `vfs`, with the
  /// edges that `trace` saw in a second color. The trace must be heard by the runtime (see
  /// ObserverChain). `vfs` and `trace` must outlive the overlay.
  void set_story(const Vfs& vfs, const TraceRecorder& trace);

  /// Turns on the "Profiler" window: the frame-time graph, the counters and the slowest loads of
  /// `profiler`, which must outlive the overlay or be reset first (null).
  void set_profiler(const Profiler* profiler) { profiler_ = profiler; }

  /// Turns on the "Translations" window: the entries of the label the story is in, in the language
  /// that is shown, each with a text field. An edit is saved to the `.po` file and shows at once.
  /// `editor` must outlive the overlay or be reset first (null).
  void set_translations(TranslationEditor* editor) { translations_ = editor; }
  /// Turns on the "Screen editor" window (engine/tools/screen_editor.hpp): with its "Pick" box
  /// on, a left click on a node of a shown screen selects it instead of going to the game. The
  /// outline of the selected node is drawn with its rectangle. `viewport` says where the canvas
  /// is in the window now. `editor` must outlive the overlay.
  void set_screen_editor(ScreenEditor* editor, std::function<Viewport()> viewport);
  /// The "Pick" box of the screen editor window.
  void set_pick_mode(bool on) { pick_ = on; }

  /// Builds this frame's windows and gives them to the renderer, which draws them in its
  /// end_frame() over the last pass. Call it once per frame after the game's passes. Does
  /// nothing while hidden. `window_pixels` is the size of that pass's target.
  void draw(Size window_pixels);

 private:
  static constexpr std::size_t kInputBytes = 512;
  static constexpr std::size_t kLevels = 3;

  void draw_console();
  void draw_inspectors();
  void draw_store_tree(const std::vector<StoreRow>& rows);
  void draw_scene_tab();
  void draw_tweens_tab();
  void draw_audio_tab();
  void draw_labels_tab();
  void draw_story_graph();
  void draw_profiler();
  void draw_translations();
  void draw_screen_editor();
  void draw_editor_fields(ScreenEditor& editor);
  void run_input();

  std::unique_ptr<ImguiBackend> backend_;
  Platform* platform_;
  ScriptRuntime* runtime_;
  Scene* scene_ = nullptr;
  Audio* audio_ = nullptr;
  std::string selected_tag_;
  LogBuffer* log_;
  std::chrono::steady_clock::time_point last_frame_ = std::chrono::steady_clock::now();
  bool visible_ = false;
  bool text_input_ = false;
  bool focus_input_ = false;
  std::size_t shown_total_ = 0;
  std::array<bool, kLevels> show_level_{true, true, true};
  std::array<char, kInputBytes> input_{};
  const Vfs* vfs_ = nullptr;
  const TraceRecorder* trace_ = nullptr;
  StoryGraph graph_;
  std::size_t trace_revision_ = 0;
  const Profiler* profiler_ = nullptr;
  TranslationEditor* translations_ = nullptr;
  // The text being typed for each entry (by key), and the language they are for.
  std::map<std::string, std::string> translation_drafts_;
  std::string translation_language_;
  std::string translation_problem_;
  ScreenEditor* screen_editor_ = nullptr;
  std::function<Viewport()> viewport_;
  bool pick_ = false;
  std::string editor_message_;
};

}  // namespace vnmaki
