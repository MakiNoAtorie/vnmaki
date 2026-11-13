#pragma once

// Hot reload (docs/SPEC.md section 18): applies a batch of changed project files to the running
// game. FileWatcher makes the batches; the host calls apply() between frames.

#include <functional>
#include <map>
#include <span>
#include <string>
#include <string_view>

#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/scene.hpp"
#include "engine/tools/log_buffer.hpp"
#include "engine/ui/ui_host.hpp"
#include "engine/ui/ui_node.hpp"

namespace vnmaki {

/// The name of the banner screen.
inline constexpr std::string_view kHotReloadScreenName = "vnmaki:hot_reload";

/// The red banner at the top of a canvas of `canvas` pixels, showing `text`.
[[nodiscard]] UiNode make_hot_reload_banner(Size canvas, std::string_view text);

/// What a changed file does:
///   - A `.lua` file: ScriptRuntime::reload_scripts() (all scripts, screens and styles), then the
///     story is replayed to where it was. A script error keeps the old scripts.
///   - An image (`.png`, `.jpg`, `.jpeg`, `.webp`): Scene::reload_image().
///   - `shaders/<name>.frag.hlsl`: Scene::recompile_shader(). A compile error keeps the old one.
/// Other files are ignored. `persistent` does not change and no rollback step is made. An error
/// goes to the log as an error entry and shows a red banner (a screen of the UiHost) until the
/// file works again; the banner names the first error and counts the others.
class HotReload {
 public:
  /// All of them must outlive this.
  HotReload(ScriptRuntime& runtime, Scene& scene, JobSystem& jobs, UiHost& ui, LogBuffer& log)
      : runtime_(&runtime), scene_(&scene), jobs_(&jobs), ui_(&ui), log_(&log) {}

  /// Called after the scripts were reloaded and the story replayed, so the host can show again
  /// the screens that it opened itself.
  void set_on_scripts_reloaded(std::function<void()> callback) {
    on_scripts_reloaded_ = std::move(callback);
  }

  /// Applies one batch: the changed paths from the project folder, with `/` separators.
  void apply(std::span<const std::string> paths);

  /// The text of the banner; empty when nothing failed.
  [[nodiscard]] const std::string& banner() const { return banner_; }

 private:
  // Records the outcome for `key`: an empty `problem` clears its error.
  void report(const std::string& key, const std::string& problem);
  void sync_banner();

  ScriptRuntime* runtime_;
  Scene* scene_;
  JobSystem* jobs_;
  UiHost* ui_;
  LogBuffer* log_;
  std::function<void()> on_scripts_reloaded_;
  std::map<std::string, std::string> errors_;  // file (or "scripts") -> the error
  std::string banner_;
  std::string shown_;  // the text of the banner screen in the host
};

}  // namespace vnmaki
