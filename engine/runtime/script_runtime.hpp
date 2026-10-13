#pragma once

// The script runtime (docs/SPEC.md section 7): one Lua 5.4 VM, the script
// loader, the label registry and the coroutine scheduler.
//
// Each label runs as a Lua coroutine. Blocking calls (`say`, `narrate`, later
// `menu` and `pause`) suspend it and hand the engine a BlockingRequest; the
// engine resumes it with advance(). `jump`, `call` and `ret` move between
// labels. Lua itself is private to the runtime: no header includes sol2 or Lua.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace vnmaki {

class Vfs;

/// `say(who, text)`: a line spoken by a named character.
struct SayRequest {
  std::string speaker;
  std::string text;

  friend bool operator==(const SayRequest&, const SayRequest&) = default;
};

/// `narrate(text)` (or `say(nil, text)`): a line with no speaker.
struct NarrateRequest {
  std::string text;

  friend bool operator==(const NarrateRequest&, const NarrateRequest&) = default;
};

/// What a suspended label waits for. Every alternative is one kind of yield
/// point; later tickets add menus and pauses.
using BlockingRequest = std::variant<SayRequest, NarrateRequest>;

/// Where a script is: the running label and how many yield points it has
/// reached since it was entered (docs/SPEC.md section 8, "Position and
/// replay"). While waiting at the first `say` of a label, step is 1.
struct ScriptPosition {
  std::string label;
  std::uint32_t step = 0;

  friend bool operator==(const ScriptPosition&, const ScriptPosition&) = default;
};

enum class RunState : std::uint8_t {
  kIdle,      ///< Scripts may be loaded; no label has started.
  kWaiting,   ///< Suspended at a yield point; request() says what for.
  kFinished,  ///< The story ended: the outermost label returned.
  kFailed,    ///< A script error stopped the story; error() says why.
};

/// A script could not be loaded: unreadable, not valid Lua, or its top-level
/// code raised an error. The message names the script (and line, when Lua
/// knows it).
class ScriptError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Receives what the scripts ask the engine for, in order.
class ScriptObserver {
 public:
  ScriptObserver() = default;
  ScriptObserver(const ScriptObserver&) = default;
  ScriptObserver(ScriptObserver&&) = default;
  ScriptObserver& operator=(const ScriptObserver&) = default;
  ScriptObserver& operator=(ScriptObserver&&) = default;
  virtual ~ScriptObserver() = default;

  /// A label was entered through start(), `jump` or `call`.
  virtual void on_label_entered(std::string_view /*label*/) {}
  /// A called label returned, so `label` (its caller) runs again.
  virtual void on_label_resumed(std::string_view /*label*/) {}
  /// The running label reached a yield point and waits at `position`.
  virtual void on_blocked(const BlockingRequest& /*request*/, const ScriptPosition& /*position*/) {}
  /// A script called `print`.
  virtual void on_print(std::string_view /*text*/) {}
  /// A dev-mode warning, e.g. a new global outside `store`.
  virtual void on_warning(std::string_view /*message*/) {}
};

struct ScriptRuntimeOptions {
  /// Dev mode adds checks that cost time, e.g. warning when a script writes a
  /// new global instead of using `store`.
  bool dev_mode = true;
  /// Upper bound on the memory the Lua VM may hold, in bytes; 0 means no
  /// limit. Going over it is a script error ("not enough memory").
  std::size_t memory_limit_bytes = 0;
  /// Upper bound on Lua VM instructions per run of script code (loading one
  /// script, or start()/advance() up to the next yield point, across every
  /// label it jumps to or calls); 0 means no limit. Going over it is a script
  /// error, so an endless loop fails instead of hanging the engine. Counted
  /// in blocks of up to 1000 instructions, so the bound is approximate.
  std::uint32_t instruction_limit = 0;
  /// Receives the scripts' requests; may be null.
  ScriptObserver* observer = nullptr;
};

/// One Lua VM running a game's scripts.
///
/// Game scripts see a restricted standard library: base (without `dofile`,
/// `loadfile`; `load` only accepts source text), `string` (without `dump`),
/// `table`, `math` (with a stub `math.random`, no `math.randomseed`), `utf8`,
/// `coroutine` and an `os` with only `clock`, `date`, `difftime` and `time`.
/// There is no `io`, `debug`, `package` or `require`. Files are read through
/// the `vfs` table (`vfs.read`, `vfs.exists`, `vfs.list`), which uses the
/// engine Vfs. `print` goes to ScriptObserver::on_print.
///
/// The engine API: `label(name, fn)`, `jump(name)`, `call(name)`, `ret()`,
/// `say(who, text)`, `narrate(text)` and the `store` table.
class ScriptRuntime {
 public:
  /// `vfs` must outlive the runtime.
  explicit ScriptRuntime(const Vfs& vfs, ScriptRuntimeOptions options = {});
  ScriptRuntime(const ScriptRuntime&) = delete;
  ScriptRuntime(ScriptRuntime&&) = delete;
  ScriptRuntime& operator=(const ScriptRuntime&) = delete;
  ScriptRuntime& operator=(ScriptRuntime&&) = delete;
  ~ScriptRuntime();

  /// Reads the script at virtual path `path` through the Vfs and runs its
  /// top-level code, which registers labels. Throws ScriptError. Only allowed
  /// before start().
  void load_script(std::string_view path);

  /// Loads every `.lua` file under `scripts/`, including subdirectories, in
  /// byte order of their paths. Throws ScriptError on the first failure.
  void load_scripts();

  [[nodiscard]] bool has_label(std::string_view name) const;

  /// Enters label `name` and runs it to its first yield point. If there is
  /// no such label the runtime fails with an error naming it.
  void start(std::string_view name);

  /// Resumes the waiting label (the player advanced past the request) and
  /// runs it to its next yield point. Does nothing unless kWaiting.
  void advance();

  [[nodiscard]] RunState state() const;
  /// What the script waits for; null unless kWaiting.
  [[nodiscard]] const BlockingRequest* request() const;
  /// The current label and step; empty label when nothing is running.
  [[nodiscard]] ScriptPosition position() const;
  /// Why the runtime failed; empty unless kFailed.
  [[nodiscard]] const std::string& error() const;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
