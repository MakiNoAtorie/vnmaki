#pragma once

// The script runtime (docs/SPEC.md section 7): one Lua 5.4 VM, the script
// loader, the label registry and the coroutine scheduler.
//
// Each label runs as a Lua coroutine. Blocking calls (`say`, `narrate`, `menu`,
// later `pause`) suspend it and hand the engine a BlockingRequest; the engine
// resumes it with advance() or choose(). `jump`, `call` and `ret` move between
// labels. Lua itself is private to the runtime: no header includes sol2 or Lua.
//
// State and replay (docs/SPEC.md section 8): game state lives in the `store`
// table. Entering a label takes a LabelSnapshot and starts an input log;
// replay() restores a snapshot and runs the label again, silently, to a step.

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

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

/// `menu { ... }`: the player picks one of the options. Answered with
/// ScriptRuntime::choose().
struct MenuRequest {
  std::vector<std::string> options;

  friend bool operator==(const MenuRequest&, const MenuRequest&) = default;
};

/// What a suspended label waits for. Every alternative is one kind of yield
/// point; later tickets add pauses.
using BlockingRequest = std::variant<SayRequest, NarrateRequest, MenuRequest>;

/// The answer to a MenuRequest: the chosen option, counted from 0.
struct MenuChoice {
  std::size_t index = 0;

  friend bool operator==(const MenuChoice&, const MenuChoice&) = default;
};

/// One entry of the input log: an answer the player gave that a replay must
/// give again. Advancing past a line carries no data, so it is not logged.
using InputResult = std::variant<MenuChoice>;

/// A table inside a StoreSnapshot, by its index in StoreSnapshot::tables.
struct StoreTableRef {
  std::uint32_t index = 0;

  friend auto operator<=>(const StoreTableRef&, const StoreTableRef&) = default;
};

/// A value kept in `store`. Keys are never StoreTableRef.
using StoreValue = std::variant<bool, std::int64_t, double, std::string, StoreTableRef>;

struct StoreTable {
  /// Key and value of every field, sorted by key (booleans, then integers,
  /// floats and strings, each in ascending order).
  std::vector<std::pair<StoreValue, StoreValue>> entries;

  friend bool operator==(const StoreTable&, const StoreTable&) = default;
};

/// A copy of `store`. tables[0] is `store` itself; the tables it reaches
/// follow in the order they are first met, so two stores with the same
/// contents give equal snapshots. Tables reached twice (or in a cycle) are
/// kept once. Metatables are not kept.
struct StoreSnapshot {
  std::vector<StoreTable> tables;

  friend bool operator==(const StoreSnapshot&, const StoreSnapshot&) = default;
};

/// What the engine keeps when a label is entered, enough to run it again
/// from its start.
struct LabelSnapshot {
  std::string label;
  StoreSnapshot store;
  std::vector<std::byte> scene;  ///< SceneState::capture() at entry.

  friend bool operator==(const LabelSnapshot&, const LabelSnapshot&) = default;
};

/// The engine-side state that is snapshotted with `store` (shown displayables,
/// playing audio, screens, camera). The runtime treats it as opaque bytes.
/// Script calls that change it must also change it during a replay; only
/// their visible and audible effects check ScriptRuntime::replaying().
class SceneState {
 public:
  SceneState() = default;
  SceneState(const SceneState&) = default;
  SceneState(SceneState&&) = default;
  SceneState& operator=(const SceneState&) = default;
  SceneState& operator=(SceneState&&) = default;
  virtual ~SceneState() = default;

  /// Serializes the current state. Called at every label entry.
  [[nodiscard]] virtual std::vector<std::byte> capture() const = 0;
  /// Replaces the current state with one capture() returned. Throws
  /// std::invalid_argument, leaving the state as it was, if `state` is
  /// malformed; a replay() then fails the runtime.
  virtual void restore(std::span<const std::byte> state) = 0;
};

/// `show(tag, options)`. Fields the script left out are empty.
struct ShowRequest {
  std::string tag{};
  /// The image name: the `image` option, or the tag without it.
  std::string image{};
  std::optional<std::string> at{};  ///< A named position: left, center, right.
  std::optional<double> x{};        ///< A fraction of the virtual width.
  std::optional<double> y{};        ///< A fraction of the virtual height.
  std::optional<std::string> layer{};
};

/// What `scene`, `show` and `hide` do to the engine (engine/scene/scene.hpp
/// implements it). A call that cannot be done returns why, and the runtime
/// raises that as a script error at the calling script line; it returns an
/// empty string otherwise. Calls arrive during a replay too.
class SceneCommands {
 public:
  SceneCommands() = default;
  SceneCommands(const SceneCommands&) = default;
  SceneCommands(SceneCommands&&) = default;
  SceneCommands& operator=(const SceneCommands&) = default;
  SceneCommands& operator=(SceneCommands&&) = default;
  virtual ~SceneCommands() = default;

  [[nodiscard]] virtual std::string scene(std::string_view image) = 0;
  [[nodiscard]] virtual std::string show(const ShowRequest& request) = 0;
  virtual void hide(std::string_view tag) = 0;
};

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
  /// A `scene`, `show` or `hide` call was done. `command` is its transcript
  /// form: "scene <image>", "hide <tag>", or "show <tag>" followed by the
  /// options given, in this order: " image <name>", " at <position>",
  /// " x <number>", " y <number>", " layer <name>".
  virtual void on_scene_command(std::string_view /*command*/) {}
  /// A script called `print`.
  virtual void on_print(std::string_view /*text*/) {}
  /// A dev-mode warning, e.g. a new global outside `store`.
  virtual void on_warning(std::string_view /*message*/) {}
};

/// The seed of the script RNG unless ScriptRuntimeOptions::random_seed is set.
inline constexpr std::uint64_t kDefaultRandomSeed = 0x5EED0F7E570B5EULL;

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
  /// Receives the scripts' requests; may be null. Nothing is reported while
  /// ScriptRuntime::replaying().
  ScriptObserver* observer = nullptr;
  /// Snapshotted and restored with `store`; may be null. Must outlive the
  /// runtime.
  SceneState* scene_state = nullptr;
  /// Carries out `scene`, `show` and `hide`; may be null, and then those
  /// calls only reach the observer. Must outlive the runtime. A Scene goes
  /// here and in `scene_state`.
  SceneCommands* scene = nullptr;
  /// The first state of the script RNG (`rand`, `math.random`). The same seed,
  /// scripts and inputs always give the same story.
  std::uint64_t random_seed = kDefaultRandomSeed;
};

/// One Lua VM running a game's scripts.
///
/// Game scripts see a restricted standard library: base (without `dofile`,
/// `loadfile`; `load` only accepts source text), `string` (without `dump`),
/// `table`, `math` (`math.random` is the engine RNG, no `math.randomseed`), `utf8`,
/// `coroutine` and an `os` with only `clock`, `date`, `difftime` and `time`.
/// There is no `io`, `debug`, `package` or `require`. Files are read through
/// the `vfs` table (`vfs.read`, `vfs.exists`, `vfs.list`), which uses the
/// engine Vfs. `print` goes to ScriptObserver::on_print.
///
/// The engine API: `label(name, fn)`, `jump(name)`, `call(name)`, `ret()`,
/// `say(who, text)`, `narrate(text)`, `menu { "option", ... }` (returns the
/// number of the chosen option, from 1), `rand` (the same function as
/// `math.random`) and the `store` table. Visuals: `scene(image)`,
/// `show(tag, { image =, at =, x =, y =, layer = })` and `hide(tag)`; option
/// keys they do not know are ignored (later tickets add `with`, `outfit`...).
/// Text: `say` and `narrate` take inline tags (docs/TEXT_TAGS.md), and a bad
/// tag is a script error at that line. `on_link(fn)` sets the function that
/// activate_link() calls.
///
/// `store` is owned by the engine: scripts change its fields but cannot
/// replace it. It keeps booleans, numbers, strings and tables with such keys
/// and values; anything else fails the story at the next label entry. The RNG
/// state is the integer `store.__rng`.
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
  /// runs it to its next yield point. Does nothing unless kWaiting at a say
  /// or narrate.
  void advance();

  /// Answers the MenuRequest the label waits at with option `index` (from 0),
  /// logs the choice and runs the label to its next yield point. Does nothing
  /// unless kWaiting at a menu. Throws std::out_of_range for a bad index.
  void choose(std::size_t index);

  /// The player activated the part of the text on screen that has the tag
  /// `{link=id}` (find it with link_at in engine/text/text.hpp). Calls the
  /// function the script gave to `on_link` with `id` and returns true, or
  /// returns false if there is none or the runtime is not kWaiting. The
  /// function runs outside the labels: an error in it, or a blocking call,
  /// fails the runtime. A replay does not call it again.
  bool activate_link(std::string_view id);

  /// Restores `snapshot` (store and scene state) and runs its label again
  /// from the start, answering menus from `inputs`, until that label waits at
  /// yield point `step`. Until then replaying() is true and the observer
  /// hears nothing; the request at `step` is reported as usual. Step 0 enters
  /// the label as a jump would. Labels the replayed label calls run again too.
  ///
  /// The label replaces the running one; the labels that called it stay as
  /// they are. On a runtime that has not started, it becomes the outermost
  /// label (callers are not rebuilt). The runtime fails if the label is
  /// unknown, the snapshot is malformed, or the label does not reach `step`
  /// with these inputs (e.g. the script changed).
  void replay(const LabelSnapshot& snapshot, std::span<const InputResult> inputs,
              std::uint32_t step);

  /// The engine-wide replay flag: true while replay() runs script code.
  /// Subsystems check it to skip visible, audible and other outside effects.
  [[nodiscard]] bool replaying() const;

  /// The snapshot taken when the running label was entered; null when no
  /// label is running. Invalidated by start(), advance(), choose(), replay().
  [[nodiscard]] const LabelSnapshot* label_snapshot() const;
  /// The answers given since the running label was entered, including those
  /// given in labels it called. Invalidated like label_snapshot().
  [[nodiscard]] std::span<const InputResult> input_log() const;
  /// A copy of `store` as it is now. Throws ScriptError if `store` holds a
  /// value that cannot be kept (function, userdata, thread, table key).
  [[nodiscard]] StoreSnapshot store_snapshot() const;

  /// A hash of the source text of label `name` (the lines of its function,
  /// without carriage returns): the same on every platform and unchanged when
  /// other code in the script moves or changes. Empty for an unknown label.
  [[nodiscard]] std::optional<std::uint64_t> label_source_hash(std::string_view name) const;

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
