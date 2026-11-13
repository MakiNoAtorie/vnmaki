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

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/plugin/plugin_value.hpp"

struct lua_State;  // Lua's own type (global scope); only the debugger uses it

namespace vnmaki {

class PluginContext;
class Vfs;

/// One line of the NVL page (`nvl(true)`, `character { kind = "nvl" }`): who said it (empty for
/// narration), the text with its tags, and the speaker's colour (0xRRGGBB).
struct NvlLine {
  std::string speaker{};
  std::string text{};
  std::optional<std::uint32_t> color{};

  friend bool operator==(const NvlLine&, const NvlLine&) = default;
};

/// The lines on the NVL page, oldest first. The page lives in `store` (`store.__nvl`), so
/// snapshots, saves and rollback cover it.
using NvlPage = std::vector<NvlLine>;

/// `say(who, text)`: a line spoken by a named character. `color` and `voice_tag` come from a
/// `character { name =, color = "#rrggbb", voice_tag = }` table; a plain string speaker has
/// neither.
struct SayRequest {
  std::string speaker{};
  std::string text{};
  /// The speaker's name colour as 0xRRGGBB.
  std::optional<std::uint32_t> color{};
  /// Only stored: audio will use it to pick the voice.
  std::string voice_tag{};
  /// `character { auto_multiplier = }`: scales the per-character part of the auto-advance delay.
  double auto_multiplier = 1;
  /// Set for a line shown on the NVL screen: the page, with this line last.
  std::optional<NvlPage> nvl{};

  friend bool operator==(const SayRequest&, const SayRequest&) = default;
};

/// `narrate(text)` (or `say(nil, text)`): a line with no speaker.
struct NarrateRequest {
  std::string text;
  /// As SayRequest::nvl.
  std::optional<NvlPage> nvl{};

  friend bool operator==(const NarrateRequest&, const NarrateRequest&) = default;
};

/// `menu { ... }`: the player picks one of the options. Answered with
/// ScriptRuntime::choose().
struct MenuRequest {
  std::vector<std::string> options;
  /// `fixed = true`: once answered, rollback cannot go back to this menu or before it.
  bool fixed = false;
  /// Set in NVL mode: the page so far (maybe empty). The options show inside the NVL screen.
  std::optional<NvlPage> nvl{};

  friend bool operator==(const MenuRequest&, const MenuRequest&) = default;
};

/// A value a screen hands back to the script, or a property of a screen node: nil, a boolean, a
/// number or a string.
using ScreenValue = std::variant<std::monostate, bool, std::int64_t, double, std::string>;

/// One node of a screen tree, as the scripts wrote it (docs/SPEC.md section 15). `kind` is
/// `box`, `hbox`, `vbox`, `grid`, `text`, `image`, `button`, `toggle`, `slider` or `bar`; `text` is
/// the content a node was given first (a caption, an image name); `props` are the named options.
/// engine/ui/ interprets them.
struct ScreenNodeDef {
  std::string kind{};
  std::string text{};
  std::vector<std::pair<std::string, ScreenValue>> props{};
  std::vector<ScreenNodeDef> children{};

  friend bool operator==(const ScreenNodeDef&, const ScreenNodeDef&) = default;
};

/// `call_screen(name, args)`: the script waits until the screen returns a value. Answered with
/// ScriptRuntime::return_screen().
struct CallScreenRequest {
  std::string name{};
  ScreenNodeDef screen{};

  friend bool operator==(const CallScreenRequest&, const CallScreenRequest&) = default;
};

/// `wait(tag)` or `wait_all()`: the script waits until the tweens of `tag` (all tweens when `tag`
/// is empty) have ended. A wait is always a step, also when nothing is tweening, so a replay
/// counts the same steps as live play. Answered with ScriptRuntime::advance(); see
/// engine/anim/wait_presenter.hpp. Rollback skips wait steps (ScriptPosition::wait). A transition
/// is a wait too, for the tag kTransitionTag.
struct WaitRequest {
  std::string tag{};
  /// For kPauseTag: how long the pause takes.
  double seconds = 0;

  friend bool operator==(const WaitRequest&, const WaitRequest&) = default;
};

/// The tag of the WaitRequest that `pause(seconds)` makes. Nothing tweens under it: the wait
/// presenter ends it by time, a click or skip mode.
inline constexpr std::string_view kPauseTag = "@pause";

/// `movie(path, { skippable = true })`: the script plays the movie `path` over all layers
/// and waits until it has played (engine/video/movie_presenter.hpp). Answered with
/// ScriptRuntime::advance(). Like a wait, it is a step that a replay passes by itself without
/// playing it, and rollback goes back over it (ScriptPosition::wait).
struct MovieRequest {
  std::string path{};
  /// The player may end the movie early with a click. Skip mode ends it whatever this says.
  bool skippable = false;

  friend bool operator==(const MovieRequest&, const MovieRequest&) = default;
};

/// What a suspended label waits for. Every alternative is one kind of yield
/// point; later tickets add pauses.
using BlockingRequest = std::variant<SayRequest, NarrateRequest, MenuRequest, CallScreenRequest,
                                     WaitRequest, MovieRequest>;

/// The answer to a MenuRequest: the chosen option, counted from 0.
struct MenuChoice {
  std::size_t index = 0;

  friend bool operator==(const MenuChoice&, const MenuChoice&) = default;
};

/// The answer to a CallScreenRequest: what the screen returned.
struct ScreenResult {
  ScreenValue value{};

  friend bool operator==(const ScreenResult&, const ScreenResult&) = default;
};

/// A write to `store` that a screen made through a binding (`bind(store, key)`) while the label
/// waited. A replay makes it again at the same wait, before the answer that follows.
struct StoreWrite {
  std::string key;
  ScreenValue value{};
  std::uint32_t wait = 0;  // the wait, counted from 1 in the log of the label that holds it

  friend bool operator==(const StoreWrite&, const StoreWrite&) = default;
};

/// One entry of the input log: an answer the player gave that a replay must
/// give again. Advancing past a line carries no data, so it is not logged.
using InputResult = std::variant<MenuChoice, ScreenResult, StoreWrite>;

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

/// Several SceneStates snapshotted as one, so every subsystem with state takes part in the
/// label-entry snapshot: put one of these in ScriptRuntimeOptions::scene_state. The parts must
/// outlive it. A snapshot keeps the parts in the order given here, so that order is part of the
/// save format.
class CompositeSceneState final : public SceneState {
 public:
  explicit CompositeSceneState(std::vector<SceneState*> parts);

  [[nodiscard]] std::vector<std::byte> capture() const override;
  /// Empty `state` (a snapshot taken without scene state) restores every part from empty bytes.
  /// If a part rejects its bytes, the parts restored before it are put back.
  void restore(std::span<const std::byte> state) override;

 private:
  std::vector<SceneState*> parts_;
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
  /// The `movie` option: shows the movie at this Vfs path (the movie file's name, with its
  /// extension) instead of an image; `image` is then the tag. `loop` is the option of that name.
  std::optional<std::string> movie{};
  bool loop = false;
  /// The `displayable` option: shows a displayable of this plugin type (engine/plugin/plugin.hpp)
  /// instead of an image; `image` is then the tag. `plugin_params` is the `params` option.
  std::optional<std::string> displayable{};
  PluginParams plugin_params{};
  /// For a layered sprite: the groups the script named and the values it chose, in the order of
  /// the definition's groups. Other groups keep their values.
  std::vector<std::pair<std::string, std::string>> attributes{};
  /// Set while the runtime replays: an attribute change does not crossfade.
  bool replaying = false;
};

/// `layered(name, options)`: a character whose image is composed of layers. `show` of `name`
/// shows it. Value names and images are in the order the script gave them (sorted by name).
struct LayeredRequest {
  /// The images of one group, in the order they draw.
  struct Group {
    std::string name{};
    /// A group of one fixed image has one value with an empty name.
    std::vector<std::pair<std::string, std::string>> values{};  ///< (value, image name)
  };

  std::string name{};
  std::vector<Group> groups{};                                  ///< From the back to the front.
  std::vector<std::pair<std::string, std::string>> defaults{};  ///< (group, value)
  double crossfade = 0;  ///< Seconds an attribute change takes; 0 for a cut.

  /// `lip_sync`: `group` shows the value in `frames` for the level of the voice with `voice_tag`
  /// (the sprite's name if empty). A level below `thresholds[0]` is the first frame, and so on.
  struct LipSync {
    std::string group{};
    std::vector<std::string> frames{};
    std::vector<double> thresholds{};
    std::string voice_tag{};
  };
  std::optional<LipSync> lip_sync{};
};

/// `animation(name, options)`: frames from `frames` (image names), or from the `columns` x `rows`
/// cells of the image `sheet`, read row by row. `show` of `name` shows the animation, and it can
/// be the value of a layered group. Fields the script left out are empty.
struct AnimationRequest {
  std::string name{};
  std::vector<std::string> frames{};
  std::string sheet{};
  std::optional<double> columns{};
  std::optional<double> rows{};
  std::optional<double> count{};  ///< The cells that are frames; all of them by default.
  std::optional<double> fps{};
  bool loop = true;  ///< False: the last frame stays when the animation has played once.
};

/// A uniform of a shader by name and its value.
using UniformValue = std::pair<std::string, double>;

/// `show(tag, { shader =, uniforms = })` or `layer(name, { shader =, uniforms = })`. Fields the
/// script left out are empty.
struct ShaderRequest {
  /// A displayable's tag, or with `layer` the name of a layer.
  std::string target{};
  bool layer = false;
  /// The shader's name; an empty name removes the shader. Without it the target keeps its shader.
  std::optional<std::string> shader{};
  std::vector<UniformValue> uniforms{};
};

/// `transition(name, options)`, or the `with` option of `scene`, `show` and `hide`. Fields the
/// script left out are empty.
struct TransitionRequest {
  /// A built-in transition (engine/anim/transition.hpp) or the name of a declared game shader.
  std::string name{};
  std::optional<double> duration{};      ///< Seconds.
  std::optional<std::uint32_t> color{};  ///< 0xRRGGBB: what `fade` goes through.
  std::optional<std::string> mask{};     ///< An image name: the mask of `mask`.
  std::optional<double> softness{};      ///< The soft edge of `wipe_*` and `mask`, from 0 to 1.
  std::vector<UniformValue> uniforms{};  ///< The uniforms of a custom shader, but "progress".
  /// Set while the runtime replays: the transition is checked and not shown.
  bool replaying = false;
};

/// The tag of the WaitRequest that a transition makes: the script blocks like `wait`.
inline constexpr std::string_view kTransitionTag = "@transition";

/// `tween(tag, options)`, or the group `sequence{...}` or `parallel{...}` of tweens. Fields the
/// script left out are empty. Positions are fractions of the virtual resolution, as for `show`;
/// `anchor` and `crop` are fractions of the displayable.
struct TweenRequest {
  enum class Kind : std::uint8_t { kTween, kSequence, kParallel };

  Kind kind = Kind::kTween;
  std::string tag{};  ///< The displayable a tween changes.
  std::optional<double> x{};
  std::optional<double> y{};
  std::optional<double> anchor_x{};
  std::optional<double> anchor_y{};
  std::optional<double> scale_x{};
  std::optional<double> scale_y{};
  std::optional<double> rotation{};  ///< Degrees, clockwise.
  std::optional<double> alpha{};
  std::optional<std::uint32_t> tint{};  ///< 0xRRGGBB.
  std::optional<double> crop_x{};
  std::optional<double> crop_y{};
  std::optional<double> crop_width{};
  std::optional<double> crop_height{};
  /// Shader uniforms by name, of the shader the displayable has.
  std::vector<UniformValue> uniforms{};
  double duration = 0.5;  ///< Seconds.
  double delay = 0;       ///< Seconds.
  std::string ease{"linear"};
  bool loop = false;
  bool yoyo = false;
  std::vector<TweenRequest> children{};  ///< The members of a group.
  /// Set on the outermost request while the runtime replays: tweens that end go to their end
  /// value at once, and loops start at time 0.
  bool replaying = false;
  /// As `replaying`, for a request that is not a replay: a camera move that is instant.
  bool instant = false;
};

/// `shake { strength, duration }`: the camera shakes for `duration` seconds, from `strength`
/// down to nothing. The shake is a fixed function of time; it is not part of any snapshot.
struct ShakeRequest {
  double strength = 0;    ///< The largest offset, as a fraction of the virtual height.
  double duration = 0.5;  ///< Seconds.
  /// Set while the runtime replays, or when motion is reduced: the request is checked and
  /// nothing shakes.
  bool replaying = false;
};

/// A point or a direction in a 3D scene (x, y, z).
using Vec3Option = std::array<double, 3>;

/// `scene3d`, `model`, `light`, `camera(scene, {...})`, `hide3d` and `play`
/// (engine/model3d/scene3d.hpp).
/// Fields the script left out are empty and keep their values; a new object starts with defaults.
struct Scene3dRequest {
  enum class Kind : std::uint8_t {
    kScene,   ///< `scene3d(scene, { layer })`: makes the scene, or moves it to `layer`.
    kModel,   ///< `model(name, path, { scene, ... })`.
    kLight,   ///< `light(scene, name, { kind, ... })`.
    kCamera,  ///< `camera(scene, { position, look_at, fov })`.
    kRemove,  ///< `hide3d(scene, name)`: removes a model or a light.
    kPlay,    ///< `play("@3d.<scene>.<name>", clip, { loop, fade, speed, seek })`.
  };

  Kind kind = Kind::kScene;
  std::string scene{};  ///< The tag of the 3D scene.
  std::string name{};   ///< The model's tag, or the light's name.
  std::optional<std::string> layer{};
  std::string path{};  ///< The Vfs path of the model file, with its extension.
  /// "unlit", "toon" or "pbr", or the name of a declared game shader.
  std::optional<std::string> shader{};
  std::vector<UniformValue> uniforms{};  ///< Uniforms of the model's game shader.
  std::optional<std::string> light_kind{};  ///< "directional", "point" or "ambient".
  std::optional<Vec3Option> position{};   ///< Model, point light, camera.
  std::optional<Vec3Option> rotation{};   ///< Model, in degrees.
  std::optional<Vec3Option> direction{};  ///< Directional light: the way the light travels.
  std::optional<Vec3Option> look_at{};    ///< Camera.
  std::optional<double> scale{};          ///< Model: uniform scale.
  std::optional<double> fov{};            ///< Camera: vertical field of view in degrees.
  std::optional<std::uint32_t> color{};   ///< Light: 0xRRGGBB.
  std::optional<double> intensity{};      ///< Light.
  std::string clip{};                     ///< Play: the clip's name.
  bool loop = false;                      ///< Play.
  double fade = 0;                        ///< Play: seconds of crossfade.
  double speed = 1;                       ///< Play.
  double seek = 0;                        ///< Play: seconds into the clip to start at.
  /// Model: the morph target that follows the voice (`lip_sync = { morph =, voice_tag = }`), and
  /// the tag of that voice (the model's tag if empty).
  std::optional<std::string> lip_morph{};
  std::string lip_voice_tag{};
  /// Set while the runtime replays: a model file is not parsed again, only checked for.
  bool replaying = false;
};

/// What `scene`, `show`, `hide`, `tween` and `wait` do to the engine (engine/scene/scene.hpp
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

  /// Defines a layered sprite. Returns why it cannot (a missing image, an unknown default, ...),
  /// as for `show`. The default does nothing, for implementations without layered sprites.
  [[nodiscard]] virtual std::string layered(const LayeredRequest& /*request*/) { return {}; }

  /// Defines a frame animation. Returns why it cannot (a missing image, a bad rate, ...), as for
  /// `show`. The default does nothing, for implementations without frame animation.
  [[nodiscard]] virtual std::string animation(const AnimationRequest& /*request*/) { return {}; }

  /// Checks that the movie file `path` can be played, before `movie(path)` blocks. Returns why
  /// not (missing, not WebM with AV1, ...), as for `show`. The default accepts every file, for
  /// implementations that do not play movies. Not called during a replay.
  [[nodiscard]] virtual std::string check_movie(std::string_view /*path*/) { return {}; }

  /// Starts the tweens. Returns why it cannot (an unknown tag or easing, ...), as for `show`. The
  /// defaults do nothing, for implementations without an animation system.
  [[nodiscard]] virtual std::string tween(const TweenRequest& /*request*/) { return {}; }
  /// Puts the tweens of `tag` (all when empty) that end at their end values: a click or skip
  /// ends a `wait`. Frame animations that end go to their last frame.
  virtual void finish_tweens(std::string_view /*tag*/) {}
  /// True while a tween that ends changes `tag`, or an animation that ends plays on it (any tag
  /// when empty): what `wait` waits for.
  [[nodiscard]] virtual bool tweening(std::string_view /*tag*/) const { return false; }

  /// `shader(name, { "uniform", ... })`: declares the game shader `shaders/<name>.frag.hlsl` and
  /// the floats of its constant buffer, in order. Declaring a shader again with the same uniforms
  /// does nothing. Returns why it cannot (a missing file, a compile error with its file and
  /// line). The default does nothing, for implementations that cannot draw shaders.
  [[nodiscard]] virtual std::string declare_shader(std::string_view /*name*/,
                                                   std::span<const std::string> /*uniforms*/) {
    return {};
  }
  /// Sets the shader and uniform values of a displayable or a layer. Returns why it cannot (an
  /// unknown tag, shader or uniform).
  [[nodiscard]] virtual std::string set_shader(const ShaderRequest& /*request*/) { return {}; }

  /// Checks a transition and starts it: it mixes the frame as it was before the changes of this
  /// step with the frame after them (engine/anim/transition.hpp). Returns why it cannot (an
  /// unknown name or a bad option), as for `show`. The runtime then waits like `wait`: the tag
  /// of that wait is kTransitionTag (`tweening(kTransitionTag)` is true while it runs, and
  /// `finish_tweens(kTransitionTag)` ends it). The default does nothing, for implementations
  /// that cannot draw transitions.
  [[nodiscard]] virtual std::string transition(const TransitionRequest& /*request*/) { return {}; }

  /// `layer(name, { parallax = f })`: the pan of the layer is `f` times the camera's pan. Returns
  /// why it cannot (an unknown layer, one the camera does not move, a number out of range). The
  /// default does nothing, for implementations without a camera.
  [[nodiscard]] virtual std::string set_parallax(std::string_view /*layer*/, double /*factor*/) {
    return {};
  }
  /// Starts a camera shake. `tweening("@camera")` is true while it runs, and
  /// `finish_tweens("@camera")` ends it. The default does nothing.
  [[nodiscard]] virtual std::string shake(const ShakeRequest& /*request*/) { return {}; }

  /// Makes or changes a 3D scene, a model, a light or a camera. Returns why it cannot (an
  /// unknown scene, a bad file, a number out of range). The default does nothing, for
  /// implementations without 3D.
  [[nodiscard]] virtual std::string scene3d(const Scene3dRequest& /*request*/) { return {}; }
};

/// What `show_screen`, `hide_screen` and `call_screen` need from the engine (engine/ui/ui_host.hpp
/// implements it). A call that cannot be done returns why, and the runtime raises that as a
/// script error at the calling script line; it returns an empty string otherwise. Calls arrive
/// during a replay too, so showing a screen that is already shown must replace it, and hiding one
/// that is not shown must do nothing.
class ScreenCommands {
 public:
  ScreenCommands() = default;
  ScreenCommands(const ScreenCommands&) = default;
  ScreenCommands(ScreenCommands&&) = default;
  ScreenCommands& operator=(const ScreenCommands&) = default;
  ScreenCommands& operator=(ScreenCommands&&) = default;
  virtual ~ScreenCommands() = default;

  /// Called for every tree before show_screen or call_screen uses it: returns why `screen`
  /// cannot be shown, without showing it.
  [[nodiscard]] virtual std::string check_screen(std::string_view name,
                                                 const ScreenNodeDef& screen) = 0;
  [[nodiscard]] virtual std::string show_screen(std::string_view name,
                                                const ScreenNodeDef& screen) = 0;
  virtual void hide_screen(std::string_view name) = 0;
};

/// `music(name, options)`: `name` loops on `channel`, replacing the music there.
struct MusicRequest {
  std::string name{};
  std::string channel{"music"};
  double fade = 0;                     ///< Seconds the old and new music cross-fade.
  std::optional<double> loop_start{};  ///< Seconds into the file; overrides the file's tags.
  std::optional<double> loop_end{};    ///< Seconds into the file; overrides the file's tags.
  /// What the captions screen says while the music starts (docs/SPEC.md section 17); empty for
  /// none. The runtime leaves it empty during a replay.
  std::string caption{};
};

/// `sound(name, options)`: `name` plays once on `channel`, over whatever plays there.
struct SoundRequest {
  std::string name{};
  std::string channel{"sound"};
  /// What the captions screen says for the length of the sound; empty for none. The runtime does
  /// not call `sound` during a replay.
  std::string caption{};
};

/// What `music`, `sound`, `voice`, `stop` and `volume` do to the engine (engine/audio/audio.hpp
/// implements it). As for SceneCommands, a call that cannot be done returns why and the runtime
/// raises that at the calling script line.
///
/// `music`, `stop` and `volume` change snapshotted state, so they arrive during a replay too.
/// `sound` and `voice` are one-shots: the runtime does not call them while replaying.
class AudioCommands {
 public:
  AudioCommands() = default;
  AudioCommands(const AudioCommands&) = default;
  AudioCommands(AudioCommands&&) = default;
  AudioCommands& operator=(const AudioCommands&) = default;
  AudioCommands& operator=(AudioCommands&&) = default;
  virtual ~AudioCommands() = default;

  [[nodiscard]] virtual std::string music(const MusicRequest& request) = 0;
  [[nodiscard]] virtual std::string sound(const SoundRequest& request) = 0;
  [[nodiscard]] virtual std::string voice(std::string_view name) = 0;
  /// Names the voice that plays now `tag`: the `voice_tag` of the character whose `say` follows a
  /// `voice` call. Lip sync follows the level of that tag. The default does nothing.
  virtual void tag_voice(std::string_view /*tag*/) {}
  [[nodiscard]] virtual std::string stop(std::string_view channel, double fade) = 0;
  [[nodiscard]] virtual std::string volume(std::string_view channel, double volume) = 0;
};

/// A script's call of `save(slot)`, `load(slot)`, `quick_save()` or `quick_load()`: the runtime
/// only queues it (take_save_requests()), and the host carries it out when the story waits.
/// Calls made during a replay are dropped.
struct SaveRequest {
  bool load = false;  ///< False: write a save. True: read one.
  /// The slot name: the slot number, or "quick".
  std::string slot{};

  friend bool operator==(const SaveRequest&, const SaveRequest&) = default;
};

/// What skip does at lines.
enum class SkipMode : std::uint8_t {
  kOff,
  kSeen,  ///< Skip lines the player has read before; stop at the first unseen line.
  kAll,   ///< Skip every line. Menus and screens always stop it.
};

/// A script's call of `skip(mode)` or `auto(on)`: the runtime only queues it
/// (take_playback_requests()), and the host applies it. Calls made during a replay are dropped.
struct PlaybackRequest {
  /// Set by `skip`.
  std::optional<SkipMode> skip{};
  /// Set by `auto`.
  std::optional<bool> automatic{};
  friend bool operator==(const PlaybackRequest&, const PlaybackRequest&) = default;
};

/// Where a script is: the running label and how many yield points it has
/// reached since it was entered (docs/SPEC.md section 8, "Position and
/// replay"). While waiting at the first `say` of a label, step is 1.
struct ScriptPosition {
  std::string label;
  std::uint32_t step = 0;
  /// The step is a `wait`. Rollback goes back over it, to the previous say or menu.
  bool wait = false;

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
  /// A replay (a load, a rollback) starts, before any label of it is reported. The label that
  /// runs next was not reached by a `jump` or `call` from the label that ran before.
  virtual void on_replay() {}
  /// The running label reached a yield point and waits at `position`.
  virtual void on_blocked(const BlockingRequest& /*request*/, const ScriptPosition& /*position*/) {}
  /// A `scene`, `show`, `hide`, `tween`, `sequence` or `parallel` call was done. `command` is its
  /// transcript form: "scene <image>", "hide <tag>", "tween <tag>", "sequence", "parallel", or
  /// "show <tag>" followed by the options given, in this order: " image <name>", " at <position>",
  /// " x <number>", " y <number>", " layer <name>", " movie <path>" (and " loop"), the layered
  /// attributes and " shader <name>".
  /// A `layer(name, options)` call is "layer <name>", with " shader <name>" if it set one.
  virtual void on_scene_command(std::string_view /*command*/) {}
  /// A `music`, `sound`, `voice`, `stop` or `volume` call was done. `command` is its transcript
  /// form, with the options given in this order:
  ///   "music <name>" " channel <name>" " fade <seconds>" " loop_start <seconds>"
  ///       " loop_end <seconds>"
  ///   "sound <name>" " channel <name>"
  ///   "voice <name>"
  ///   "stop <channel>" " fade <seconds>"
  ///   "volume <channel> <number>"
  virtual void on_audio_command(std::string_view /*command*/) {}
  /// A script called `print`.
  virtual void on_print(std::string_view /*text*/) {}
  /// A dev-mode warning, e.g. a new global outside `store`.
  virtual void on_warning(std::string_view /*message*/) {}
};

/// How many steps back the player can roll unless ScriptRuntimeOptions::rollback_depth is set.
inline constexpr std::size_t kDefaultRollbackDepth = 100;

/// How many lines the backlog shows at most unless project.toml `backlog_length` says otherwise.
inline constexpr std::size_t kDefaultBacklogLength = 100;

/// A position the player can roll back to: a say, narrate, menu or call_screen the story waited
/// at (never a `wait`). It holds all that replaying to there takes, so every step stands alone.
struct RollbackStep {
  /// The label-entry snapshot and input log of the outermost running label at that time. Its
  /// log covers the answers given in the labels it called.
  LabelSnapshot root{};
  std::vector<InputResult> inputs{};
  /// Label and step of every running label, outermost first.
  std::vector<ScriptPosition> stack{};
  /// How many yield points (waits too) the outermost label reached, counting this one, in this
  /// label and the labels it called: what replaying `root` has to count up to. At least 1.
  std::uint32_t ordinal = 0;
  /// What the story waited for there.
  BlockingRequest request{};
  /// The name of the last `voice(name)` call before this say or menu (and after the one before);
  /// empty if there was none. The backlog plays it again.
  std::string voice{};
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
  /// How many steps back rollback can go (project.toml `rollback_depth`); 0 turns it off.
  std::size_t rollback_depth = kDefaultRollbackDepth;
  /// Receives the scripts' requests; may be null. Nothing is reported while
  /// ScriptRuntime::replaying().
  ScriptObserver* observer = nullptr;
  /// Snapshotted and restored with `store`; may be null. Must outlive the
  /// runtime. Use a CompositeSceneState for more than one subsystem.
  SceneState* scene_state = nullptr;
  /// Carries out `scene`, `show` and `hide`; may be null, and then those
  /// calls only reach the observer. Must outlive the runtime. A Scene goes
  /// here and in `scene_state`.
  SceneCommands* scene = nullptr;
  /// Carries out `show_screen` and `hide_screen`; may be null, and then those calls do nothing
  /// and no screen is checked. Must outlive the runtime.
  ScreenCommands* screens = nullptr;
  /// Carries out `music`, `sound`, `voice`, `stop` and `volume`; may be null, and then those
  /// calls only reach the observer. Must outlive the runtime. An Audio goes here, and in
  /// `scene_state` (inside a CompositeSceneState when there is a Scene too).
  AudioCommands* audio = nullptr;
  /// The C++ plugins' functions: installed as globals when the scripts load. Null means the
  /// context that the plugins linked into the executable registered in
  /// (`PluginContext::linked()`). It must outlive the runtime.
  const PluginContext* plugins = nullptr;
  /// The images (names, as for `show`) of the gallery (project.toml `gallery`). Showing one with
  /// `scene` or `show` unlocks `cg/<name>` in `persistent.unlocks`.
  std::vector<std::string> gallery{};
  /// The language codes of project.toml, the source language first, as `languages()` in scripts.
  std::vector<std::string> languages{};
  /// The virtual resolution, as the read-only global `canvas` (`canvas.width`, `canvas.height`)
  /// that screens size themselves by.
  int canvas_width = 1920;
  int canvas_height = 1080;
  /// The first state of the script RNG (`rand`, `math.random`). The same seed,
  /// scripts and inputs always give the same story.
  std::uint64_t random_seed = kDefaultRandomSeed;
  /// Set through ScriptRuntime::set_thread_hook(); see ThreadHook.
  std::function<void(lua_State*)> thread_hook{};
};

/// Called with the main thread of each new VM (the constructor, reload_scripts()) and with the
/// thread of each label (also in a replay). Only the debugger (engine/tools/debugger.hpp) sets one.
using ThreadHook = std::function<void(lua_State*)>;

/// The outcome of ScriptRuntime::eval.
struct EvalResult {
  bool ok = false;
  /// The returned values, tab-separated (empty for a statement), or the error message.
  std::string text{};
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
/// The engine API: `character { name =, color = "#rrggbb", voice_tag =, auto_multiplier = }`,
/// `label(name, fn)`, `jump(name)`, `call(name)`, `ret()`, `say(who, text)`, `narrate(text)`, `menu
/// { "option", ... }` (returns the number of the chosen option, from 1), `rand` (the same function
/// as `math.random`) and the `store` table. Visuals: `scene(image)`, `show(tag, { image =, at =, x
/// =, y =, layer =
/// })` and `hide(tag)`; option keys they do not know are ignored (later tickets add `with`,
/// `outfit`...).
/// Text: `say` and `narrate` take inline tags (docs/TEXT_TAGS.md), and a bad
/// tag is a script error at that line. `on_link(fn)` sets the function that
/// activate_link() calls.
/// Audio: `music(name, { channel =, fade =, loop_start =, loop_end = })`,
/// `sound(name, { channel = })`, `voice(name)`, `stop(channel, { fade = })` and
/// `volume(channel, number)`; times are in seconds.
/// Animation: `tween(tag, { x =, y =, anchor = { x =, y = }, scale = number or { x =, y = },
/// rotation =, alpha =, tint = "#rrggbb", crop = { x =, y =, width =, height = }, duration =,
/// delay =, ease =, loop =, yoyo = })`; the units are those of `show`. `sequence { item... }` and
/// `parallel { item... }` start a group, where an item is `{ tag, <the options of tween> }` or a
/// group `{ sequence = { item... } }` or `{ parallel = { item... } }`; the group table may have
/// `delay` and `loop`. `wait(tag)` and `wait_all()` block until those tweens have ended. A tween
/// that is not waited for does not block.
/// Shaders: `shader(name, { "uniform", ... })` declares the game shader `shaders/<name>.frag.hlsl`
/// and its float uniforms, in order. `show` takes `shader = name` and `uniforms = { uniform =
/// number }`. `layer(name, { shader =, uniforms = })` sets them on a layer. `tween` takes
/// `uniforms = { uniform = number }`; the tag `@layer.<name>` tweens those of a layer.
/// Screens: `screen "name" { node... }` (or a function of the arguments that
/// returns nodes), the node constructors `box`, `hbox`, `vbox`, `text`, `image` and `button`,
/// `show_screen(name, args)`, `hide_screen(name)` and `call_screen(name, args)`, which waits for
/// the screen to return a value (a button's `returns` option).
///
/// Saves: `save(slot)` and `load(slot)` (a slot is an integer from 1; `load` with a number is a
/// slot, with a string or function it is Lua's `load`), `quick_save()` and `quick_load()`. They
/// queue a SaveRequest. `on_load(function(old_version, store) ... end)` registers the migration
/// hook that run_on_load() calls; scripts register it while they load.
///
/// Playback: `skip("seen")`, `skip("all")` and `skip(false)` (also `"off"`) set the skip mode;
/// `auto(true)` and `auto(false)` turn auto-advance on and off. They queue a PlaybackRequest.
/// `character { auto_multiplier = }` scales the auto-advance time of that character's lines.
///
/// Bindings and preferences: `bind(table, key)` is a node option that follows `store`,
/// `persistent` or `prefs` and writes it back (`toggle`, `slider`, `bar`; `grid { columns = n }`
/// lays children out in rows). `prefs` is `persistent.prefs` with defaults: text_speed (characters
/// a second, 40), auto_delay (seconds, 1), volume_music, volume_sound, volume_voice,
/// volume_ambient and volume_ui (0 to 1, 1), fullscreen (false), skip_unseen (false: the skip
/// key stops at unread text), window_opacity (0.3 to 1, 1), reduce_motion (false) and
/// self_voicing (false: speak the story and the focused control). The engine applies them.
///
/// Pauses and NVL: `pause(seconds)` is a blocking step (a WaitRequest for kPauseTag). `nvl(true)`
/// turns NVL mode on and `nvl(false)` off (and empties the page); `nvl_clear()` empties the page.
/// In NVL mode, and for a `character { kind = "nvl" }`, `say` and `narrate` add their line to the
/// page and the request carries it (SayRequest::nvl); a menu in NVL mode does too. The page is
/// `store.__nvl`. Gallery: `unlock(kind, name)` sets `persistent.unlocks["kind/name"]`;
/// `unlocked(kind, name)` reads it; `gallery_images()` lists ScriptRuntimeOptions::gallery.
///
/// `load_scripts()` loads the engine's default screens first (engine/runtime/default_screens.cpp:
/// title, say, choice, save, load, preferences, quick_menu, confirm, nvl, gallery), so a game
/// replaces one by defining a screen of that name. Then it sets the functions of the C++ plugins
/// as globals (ScriptRuntimeOptions::plugins) and loads the Lua plugins that project.toml lists
/// (`plugins/<name>/`, engine/plugin/lua_plugin.hpp), and last the scripts of the game. A plugin
/// whose engine version range does not hold, or whose files are missing, is a ScriptError.
///
/// `persistent` is a second table like `store`, but for data that outlives a playthrough (seen
/// text, unlocks, preferences). Snapshots, saves, rollback and load never change it; the host
/// keeps it in a file (persistent_snapshot(), restore_persistent()). A replay runs the label's
/// code again, so code that writes to it should be idempotent (`persistent.won = true`).
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
  /// The names of all labels, sorted.
  [[nodiscard]] std::vector<std::string> label_names() const;

  /// Enters label `name` and runs it to its first yield point. If there is
  /// no such label the runtime fails with an error naming it.
  void start(std::string_view name);

  /// Resumes the waiting label (the player advanced past the request) and
  /// runs it to its next yield point. Does nothing unless kWaiting at a say,
  /// narrate, wait or movie.
  void advance();

  /// Answers the MenuRequest the label waits at with option `index` (from 0),
  /// logs the choice and runs the label to its next yield point. Does nothing
  /// unless kWaiting at a menu. Throws std::out_of_range for a bad index.
  void choose(std::size_t index);

  /// Answers the CallScreenRequest the label waits at: `call_screen` returns `value` (nil for
  /// std::monostate). Logs the answer and runs the label to its next yield point. Does nothing
  /// unless kWaiting at a call_screen.
  void return_screen(ScreenValue value);
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

  /// Goes back one say, narrate, menu or call_screen (the previous one: `wait` steps do not
  /// count), as replay() does: the story waits there again, and the observer hears that request.
  /// Across a label boundary it replays from the label that was running then, with its callers.
  /// At a menu or screen the player can answer again; a new answer removes what was after it.
  /// `persistent` does not change. Returns false, changing nothing, if there is nowhere to go
  /// back to: the story does not wait, the history is empty or its depth is spent, or the player
  /// answered a `fixed` menu since. The runtime fails if the replay does.
  bool roll_back();
  /// True if roll_back() would go back.
  [[nodiscard]] bool can_roll_back() const;
  /// The positions roll_back() can go to, oldest first. While the story waits at a line, menu or
  /// screen, the last one is that request. A load (replay()) empties it. Invalidated like
  /// label_snapshot().
  [[nodiscard]] std::span<const RollbackStep> rollback_steps() const;

  /// Makes the running label the outermost one, so a load does not return into the labels that
  /// called the label it replaced. Call it before replay() when a story is already running.
  void drop_callers();
  /// drop_callers() that keeps the callers and the history until end_load(), so a load whose
  /// replay fails can undo them. Call end_load() after the load, whatever its result.
  void begin_load();
  /// With `undo`, the callers and the history come back under the running label (it must be
  /// the label a failed load went back to). Without it they are dropped for good.
  void end_load(bool undo);

  /// Runs the script's `on_load(old_version, store)` hook (if it registered one) on the store of
  /// `snapshot` and puts the result back into `snapshot`. The current `store` is untouched.
  /// Returns why it failed (the script raised an error or ran too long), or an empty string.
  [[nodiscard]] std::string run_on_load(LabelSnapshot& snapshot, std::string_view old_version);

  /// The save and load requests scripts made since the last call, in order.
  [[nodiscard]] std::vector<SaveRequest> take_save_requests();

  /// The `skip` and `auto` requests scripts made since the last call, in order.
  [[nodiscard]] std::vector<PlaybackRequest> take_playback_requests();
  /// The boolean, number or string in field `key` of `table` ("store", "persistent" or "prefs");
  /// nil (std::monostate) for anything else. `prefs` reads give the default of a key the player
  /// never set.
  [[nodiscard]] ScreenValue binding(std::string_view table, std::string_view key) const;
  /// Writes `value` (nil removes the key) to field `key` of `table`. Returns why it cannot (an
  /// unknown table, a value of the wrong type for a pref), or an empty string. A write to `store`
  /// is not in the input log, so a rollback or a load takes it back like any other change.
  [[nodiscard]] std::string set_binding(std::string_view table, std::string_view key,
                                        ScreenValue value);
  /// Builds screen `name` for the arguments `args` (the table the screen function gets: `props`
  /// are its named fields, `children` its list, each a table of the same form) into `tree`.
  /// Returns why it cannot, or an empty string. Runs the screen's Lua function.
  [[nodiscard]] std::string build_screen(std::string_view name, const ScreenNodeDef& args,
                                         ScreenNodeDef& tree);
  /// Sets the style override file of the player's language (SPEC section 16): the styles go back to
  /// what the scripts and `theme()` made, and the file at `path` (a `styles/...lua` script that
  /// calls `style`) runs on top of them. An empty path means none. Screens built from now on use
  /// it. Before load_scripts() it only remembers the path, and load_scripts() (also a reload)
  /// applies it. Returns why it cannot, or an empty string.
  /// `vertical` (project.toml `[vertical]`) sets `vertical = true` in the styles `say_text` and
  /// `nvl_text` after the file has run, so the default say box and NVL screen show their lines in
  /// columns; a file can also set it itself.
  [[nodiscard]] std::string set_language_styles(std::string_view path, bool vertical = false);
  /// A copy of `persistent` as it is now. Throws ScriptError like store_snapshot().
  [[nodiscard]] StoreSnapshot persistent_snapshot() const;
  /// Makes `persistent` equal to `snapshot`. Call it before load_scripts() so that scripts that
  /// set defaults at their top level see the saved values. Returns why the snapshot is
  /// malformed, or an empty string.
  [[nodiscard]] std::string restore_persistent(const StoreSnapshot& snapshot);
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

  /// Dev tools: runs `code` in the script sandbox, as a chunk of the REPL. Tries it as an
  /// expression (`return <code>`) first, then as statements. A change to `store` stays until
  /// the next rollback or load (neither is in the input log); a blocking call is an error.
  /// Never throws; a script error is `ok == false`.
  [[nodiscard]] EvalResult eval(std::string_view code);
  /// Replaces the observer (null: none). Pass one that outlives the runtime or reset it first.
  void set_observer(ScriptObserver* observer);

  /// Dev tools: `hook` is called for the VM now, for the label threads that run, and for each
  /// later VM and label thread. A reload keeps it. Empty turns it off.
  void set_thread_hook(ThreadHook hook);
  /// The thread of the running label (the top of the call stack); null if none runs.
  [[nodiscard]] lua_State* label_thread() const;

  /// Dev tools (hot reload): loads every script again, the default screens and styles with them,
  /// and replays to the position the story waits at. The new scripts run in a fresh Lua VM, so a
  /// script error (also in a `shader` call) leaves the old VM, scripts and story as they were and
  /// is the return value (`script:line: message`); the scene state is put back too. `persistent`
  /// is copied over unchanged. The story comes back through the replay of the outermost label
  /// (as roll_back() does, so `call` stacks stay) to the same yield point. If the label is gone, it
  /// goes to `start` with an empty scene and the store of a new game. If the replay cannot reach
  /// that yield point (the label got shorter, a menu is new, the code now fails), it stops at the
  /// last yield point it reached. The observer hears the request it stops at, as for a load.
  /// The rollback history stays, but the position the story waits at is not added twice; the
  /// queued save and playback requests stay. Do not call it from inside the runtime (a script,
  /// an observer) or while replaying. Invalidates label_snapshot(), input_log() and request().
  [[nodiscard]] std::string reload_scripts();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace vnmaki
