#pragma once

// The scene (docs/SPEC.md sections 7 and 9): a stack of layers holding tagged displayables. The
// script calls `scene`, `show`, `hide` and `tween` change it, snapshots capture it, and the engine
// draws it each frame.

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/anim/timeline.hpp"
#include "engine/anim/transition.hpp"
#include "engine/audio/voice_level.hpp"
#include "engine/jobs/job_system.hpp"
#include "engine/platform/geometry.hpp"
#include "engine/render/renderer.hpp"
#include "engine/render/shader_compiler.hpp"
#include "engine/runtime/script_runtime.hpp"
#include "engine/scene/camera.hpp"
#include "engine/scene/displayable.hpp"
#include "engine/scene/frame_animation.hpp"
#include "engine/scene/layered_sprite.hpp"
#include "engine/scene/movie_displayable.hpp"
#include "engine/video/movie_audio.hpp"

namespace vnmaki {

class ModelLibrary;
class PluginContext;
class Scene3dDisplayable;
class Vfs;

/// The file of image name `name`: "images/<name>.png", else ".jpg", else ".webp" (a name may hold
/// "/"). With a `language`, "images/<language>/<name>" comes first (a picture with text in that
/// language). Nothing when none exists. `vnmaki check` uses the same rule.
[[nodiscard]] std::optional<std::string> resolve_image(const Vfs& vfs, std::string_view name,
                                                       std::string_view language = {});

/// The layers, in the order they are declared and drawn: kBackground first (at the back).
enum class Layer : std::uint8_t {
  kBackground,
  kMaster,
  kCharacters,
  k3d,
  kOverlay,
  kUi,
  kTransient,
};

/// Every layer, back to front.
inline constexpr std::array kLayerOrder{
    Layer::kBackground, Layer::kMaster, Layer::kCharacters, Layer::k3d,
    Layer::kOverlay,    Layer::kUi,     Layer::kTransient,
};

/// The name scripts use: "background", "master", "characters", "3d", "overlay", "ui", "transient".
[[nodiscard]] std::string_view layer_name(Layer layer);
[[nodiscard]] std::optional<Layer> layer_from_name(std::string_view name);

/// The largest `x` or `y` a script may give, as a fraction of the virtual resolution.
inline constexpr double kMaxPositionFraction = 1'000'000.0;

/// The tag of the displayable `scene(image)` puts on the background layer.
inline constexpr std::string_view kBackgroundTag = "background";

/// `tween` addresses the uniforms of a layer's shader with the tag "@layer.<name>".
inline constexpr std::string_view kLayerTagPrefix = "@layer.";

/// What is shown: which displayable has which tag, on which layer, with which properties.
///
/// A tag names at most one displayable in the whole scene. Within a layer, displayables draw in
/// the order they were first shown. Positions are virtual pixels.
///
/// `show(tag, options)` shows image `options.image` (the tag itself without it) on
/// `options.layer` (the tag's current layer, or "characters"). `at` is "left", "center" or
/// "right": a quarter, a half or three quarters across, standing on the bottom edge (the anchor
/// is the image's bottom centre). `x` and `y` then override either coordinate as a fraction of
/// the virtual resolution (at most kMaxPositionFraction either way). A new tag starts at "center";
/// a tag already shown keeps the properties the call does not set, and its place in the drawing
/// order if the layer is the same. `scene(image)` empties the background, master and characters
/// layers and puts the image on the background with its top-left corner at the origin. `hide` of a
/// tag not shown does nothing.
///
/// `animation(name, ...)` defines a frame animation (engine/scene/frame_animation.hpp). `show` of
/// that name shows it, and it plays from frame 1 on the clock of update(). The name of an
/// animation can be a value of a layered group. An animation takes the place of an image of the
/// same name, and a layered sprite takes the place of both; so scripts define an animation before
/// the layered sprite that uses it. An animation that does not loop ends at its last frame: it is
/// what `tweening` reports while it plays, and `finish_tweens` puts it at its end. After a
/// restore it plays again from frame 1.
///
/// `show(tag, { movie = path, loop = true })` shows a movie (engine/scene/movie_displayable.hpp)
/// instead of an image; `path` is the movie file in the Vfs, with its extension. A movie loads on a
/// job and plays from time 0 on the clock of update(); with audio, its Opus track plays on the
/// `movie` channel of the MovieAudio from set_movie_audio() (which must outlive the scene) and the
/// picture follows it. A movie that does not loop is what `tweening` reports until it has played,
/// and `finish_tweens` ends it. After a restore it plays again from time 0.
///
/// `tween` changes the properties of shown displayables over time (engine/anim/timeline.hpp), as
/// update() moves the clock on. `hide` and `scene` end the tweens of the displayables they remove.
///
/// `layered(name, ...)` defines a layered sprite (engine/scene/layered_sprite.hpp). `show` of
/// that name (as the tag or as `image`) shows it, and the groups of layers that the call names
/// change; the others keep their images. A group that changes crossfades, except in a replay. A
/// definition is not in the snapshot: restore() needs it made first, which the scripts do when
/// they load.
///
/// A layered sprite with `lip_sync` shows the frame for the voice level on update(), from the
/// VoiceLevel of set_voice_level(). It is not state: restore() and a replay leave the mouth closed
/// (a replay plays no voice).
///
/// `shader(name, { "amount", ... })` declares the game shader `shaders/<name>.frag.hlsl` and
/// compiles it (engine/render/shader_compiler.hpp). A displayable or a layer then gets a shader
/// and uniform values from `show(tag, { shader =, uniforms = })` or `layer(name, { shader =,
/// uniforms = })`, and `tween` changes the values. The uniforms are floats packed into the
/// shader's constant buffer in the declared order and padded to 16 bytes. A displayable draws
/// with its shader. A layer with a shader draws into a render target of the virtual resolution,
/// and then that target draws with the shader. prepare() makes the GPU shaders and the targets.
///
/// `transition(request)` (engine/anim/transition.hpp) blends the frame as it was before the changes
/// of the step into the frame after them. The layers from the background to the overlay draw into
/// two render targets, the old frame (frozen) and the new one (live), and a shader mixes them in
/// the pass of draw()'s target; the UI and transient layers draw over the result. Nothing is
/// drawn in an extra pass without a transition. The old frame is drawn at the first change after
/// a draw(), from the state before it, so prepare() must have seen the renderer (it keeps a
/// pointer: the renderer must outlive the changes). A transition is not part of the snapshot and
/// a replay (ShowRequest::replaying, restore()) does not start one. It runs on update() and
/// ends at once with finish_tweens(kTransitionTag).
///
/// The camera (engine/scene/camera.hpp) moves the layers from the background to the overlay: the
/// UI and transient layers stay. `tween("@camera", { x, y, zoom, rotation })` changes it, so
/// `wait("@camera")` waits for a move or a shake. `set_parallax` gives a layer a factor for the
/// camera's pan. `shake` adds a fixed function of time to the pan until its duration has passed;
/// it is not in the snapshot and a replay does not start it. Layers draw through a CameraRenderer
/// when the camera moves them, so every displayable follows it.
///
/// `scene3d(name, { layer })` puts a 3D scene on a layer (engine/model3d/scene3d.hpp): a camera,
/// lights and glTF models drawn into a target with a depth buffer, which then draws like an image.
/// `model`, `light` and `camera(scene, ...)` change it; `tween("@3d.<scene>.<name>", ...)` moves a
/// model, a light or the "camera". prepare() uploads the models and makes the target, and draw()
/// renders each 3D scene in a pass of its own before the layer that holds it, then goes on in a
/// new pass on its target. A model that has a game shader needs the shader to include
/// shaders/mesh.hlsli. `play("@3d.<scene>.<model>", clip, ...)` plays a clip of a model's file
/// (skeleton and morph targets, engine/model3d/animation.hpp), update() moves it on, and `wait`
/// on the tag waits for a clip that does not loop. A model with a `lip_sync` follows the voice
/// level of set_voice_level() like a layered sprite. A restore or a replay puts a clip that does
/// not loop at its end and starts a loop at time 0.
///
/// The snapshot is a map from a part's name to its bytes, so a later feature adds a part without
/// changing the others: "displayables" (the images), "layered" (the layered sprites),
/// "animations" (the frame animations), "movies" (the movies), "tweens" (the loops), "shaders"
/// (the shader and uniform values of displayables and layers; left out when no shader is set) and
/// "camera" (the camera and the parallax of the layers; left out when they are the default),
/// "scene3d" (the 3D scenes, put back after the other displayables; left out when there are none)
/// and "plugins" (the displayables of C++ plugins: their type and params; put back last, left out
/// when there are none). It
/// keeps the properties as a replay would leave them: tweens that end are at their end values,
/// loops at the value they started from, and restore() starts each loop again at time 0.
class Scene final : public SceneState, public SceneCommands {
 public:
  /// `vfs` must outlive the scene. `virtual_size` is the virtual resolution.
  Scene(const Vfs& vfs, Size virtual_size);

  [[nodiscard]] std::string scene(std::string_view image) override;
  [[nodiscard]] std::string show(const ShowRequest& request) override;
  void hide(std::string_view tag) override;
  [[nodiscard]] std::string layered(const LayeredRequest& request) override;
  [[nodiscard]] std::string animation(const AnimationRequest& request) override;
  [[nodiscard]] std::string check_movie(std::string_view path) override;
  [[nodiscard]] std::string tween(const TweenRequest& request) override;
  void finish_tweens(std::string_view tag) override;
  [[nodiscard]] bool tweening(std::string_view tag) const override;
  [[nodiscard]] std::string declare_shader(std::string_view name,
                                           std::span<const std::string> uniforms) override;
  /// Dev tools (hot reload): compiles the declared shader `name` again from its file. Returns the
  /// compile error, leaving the old shader in use, or an empty string; an undeclared name does
  /// nothing.
  [[nodiscard]] std::string recompile_shader(std::string_view name);
  /// Dev tools (hot reload): reads the image file `path` again and puts its texture in place of
  /// the old one in every displayable that shows it. Returns why it cannot decode the file (the
  /// old texture stays), or an empty string. Does nothing for a file that was never drawn, and
  /// before the first prepare().
  [[nodiscard]] std::string reload_image(const std::string& path, JobSystem& jobs);
  [[nodiscard]] std::string set_shader(const ShaderRequest& request) override;
  [[nodiscard]] std::string transition(const TransitionRequest& request) override;
  [[nodiscard]] std::string set_parallax(std::string_view layer, double factor) override;
  [[nodiscard]] std::string shake(const ShakeRequest& request) override;
  [[nodiscard]] std::string scene3d(const Scene3dRequest& request) override;

  [[nodiscard]] std::vector<std::byte> capture() const override;
  void restore(std::span<const std::byte> state) override;

  /// The audio that movies play on. May be null (movies are then silent). It must outlive the
  /// scene, whose movies stop their audio when they go away.
  void set_movie_audio(MovieAudio* audio) { movie_audio_ = audio; }
  /// The plugins whose displayable types `show(tag, { displayable = type })` makes. Null (the
  /// default) means `PluginContext::linked()`. It must outlive the scene.
  void set_plugins(const PluginContext* plugins) { plugins_ = plugins; }
  /// The voice level that lip-synced layered sprites follow on update(). May be null: the mouths
  /// then stay closed. It must outlive the scene.
  void set_voice_level(const VoiceLevel* levels) { voice_level_ = levels; }
  /// The language whose images (`images/<language>/<name>`) the image calls look for first. It
  /// changes the lookup of calls from now on: an image that is shown keeps its file until it is
  /// shown again or the story replays.
  void set_language(std::string language) { language_ = std::move(language); }

  /// Where prepare() and update() report a file that cannot be loaded or played (an image that
  /// does not decode, a movie that fails). That displayable stays in the scene and draws nothing,
  /// and the frame goes on. Without a handler the errors are thrown.
  void set_warning_handler(std::function<void(std::string_view)> handler) {
    warn_ = std::move(handler);
  }

  /// The tags on `layer`, back to front.
  [[nodiscard]] std::vector<std::string> tags(Layer layer) const;
  [[nodiscard]] std::optional<Layer> layer_of(std::string_view tag) const;
  /// The displayable with this tag, or null. Invalidated by the next call that changes what is
  /// shown.
  [[nodiscard]] Displayable* find(std::string_view tag);
  [[nodiscard]] const Displayable* find(std::string_view tag) const;
  /// The shader and uniform values of a layer (the other properties mean nothing).
  [[nodiscard]] const DisplayProperties& layer_properties(Layer layer) const {
    return layer_state_.at(static_cast<std::size_t>(layer));
  }
  [[nodiscard]] const Timeline& timeline() const { return timeline_; }
  [[nodiscard]] const CameraState& camera() const { return camera_; }
  /// The shake that runs, or null.
  [[nodiscard]] const Shake* active_shake() const {
    return shake_.has_value() ? &*shake_ : nullptr;
  }

  /// Advances the tweens and every displayable by `seconds`.
  void update(double seconds);
  /// Loads and uploads the images of displayables that have no texture yet. Call it on the main
  /// thread before draw(), always with the same renderer (the scene keeps its textures). A file
  /// that cannot be decoded (ImageError, MovieError) goes to the warning handler once and its
  /// displayable draws nothing; without a handler it is thrown. RenderError is always thrown.
  void prepare(IRenderer& renderer, JobSystem& jobs);
  /// Draws every layer, back to front, into the renderer's current pass, whose target is
  /// `target`. A layer with a shader draws in passes of its own, and then the scene goes on in a
  /// new pass on `target`, so give the right target when a layer has a shader.
  void draw(IRenderer& renderer, TextureId target = kScreen) const;

 private:
  struct Entry {
    std::string tag;
    std::unique_ptr<Displayable> displayable;
  };
  struct Texture {
    TextureId id{};
    Size size{};
  };
  struct Slot {
    Layer layer = Layer::kBackground;
    std::size_t index = 0;
  };
  struct GameShader {
    std::string name;
    std::vector<std::string> uniforms;
    CompiledShader compiled;
    std::size_t uniform_floats = 0;  // what a draw gives the constant buffer; 0 without one
    std::optional<ShaderId> gpu;     // made by prepare()
  };
  // The transition that runs, with what the shader needs besides its clock.
  struct ActiveTransition {
    Transition clock;
    std::string mask_path;                     // the resolved mask image, or empty
    std::uint32_t shader = 0;                  // a custom transition: the number of its game shader
    std::array<float, kMaxUniforms> values{};  // a custom transition: its uniforms but "progress"
    std::size_t progress_slot = 0;             // a custom transition: the index of "progress"
    bool blank_from = false;                   // there was no frame before: the old frame is empty
  };
  // What frames_[0] holds. draw() makes it kPending: the scene is as the last frame showed it,
  // and the first change draws that frame into frames_[0] (kTaken).
  enum class OldFrame : std::uint8_t { kNone, kPending, kTaken };
  using Layers = std::array<std::vector<Entry>, kLayerOrder.size()>;
  // The shader and uniform values of each layer, as DisplayProperties so that tweens reach them.
  using LayerState = std::array<DisplayProperties, kLayerOrder.size()>;

  [[nodiscard]] std::optional<Slot> locate(std::string_view tag) const;
  [[nodiscard]] Timeline::Lookup lookup();
  // The properties of a displayable, or of a layer for the tag "@layer.<name>"; null if unknown.
  [[nodiscard]] static DisplayProperties* properties_in(Layers& layers, LayerState& layer_state,
                                                        CameraState& camera, std::string_view tag);
  [[nodiscard]] const DisplayProperties* properties_of(std::string_view tag) const;
  // The index of a uniform of the shader that `tag` has, from its name.
  [[nodiscard]] std::optional<std::size_t> uniform_index(std::string_view tag,
                                                         std::string_view name) const;
  // Reads the "displayables" part of a snapshot. Throws std::invalid_argument.
  [[nodiscard]] static Layers read_displayables(std::span<const std::byte> bytes);
  // Reads the "shaders" part into displayables that are there already. Throws
  // std::invalid_argument.
  void read_shaders(std::span<const std::byte> bytes, Layers& layers,
                    LayerState& layer_state) const;
  // Reads the "camera" part. Throws std::invalid_argument.
  [[nodiscard]] static CameraState read_camera(std::span<const std::byte> bytes);
  // declare_shader(); `force` compiles a shader that is declared with the same uniforms again.
  [[nodiscard]] std::string declare_shader_impl(std::string_view name,
                                                std::span<const std::string> uniforms, bool force);
  // The number of the declared shader `name` (DisplayProperties::shader), or 0.
  [[nodiscard]] std::uint32_t shader_id(std::string_view name) const;
  // The shader with this number, or null for 0 and unknown numbers.
  [[nodiscard]] const GameShader* shader_by_id(std::uint32_t id) const;
  // Gives displayables their GPU shaders and creates the targets of layers with a shader.
  void prepare_shaders(IRenderer& renderer);
  // Draws the layers [first, last) of kLayerOrder in the renderer's current pass, whose target is
  // `target`.
  void draw_layers(IRenderer& renderer, std::size_t first, std::size_t last,
                   TextureId target) const;
  // Draws the old frame (the scene before the first change since the last draw()) into
  // frames_[0], in a frame of its own. It does nothing unless that is due.
  void capture_old_frame();
  // Throws std::invalid_argument if a file that `layers` show is not in the Vfs.
  void check_assets(const Layers& layers) const;
  // Reads the "layered" part into `layers`. Throws std::invalid_argument.
  void read_layered(std::span<const std::byte> bytes, Layers& layers) const;
  // Reads the "animations" part into `layers`. Throws std::invalid_argument.
  void read_animations(std::span<const std::byte> bytes, Layers& layers) const;
  // Reads the "scene3d" part into `layers`. Throws std::invalid_argument.
  void read_scene3d(std::span<const std::byte> bytes, Layers& layers) const;
  // Sets the model `request.name` of a 3D scene. Returns why it cannot.
  [[nodiscard]] std::string set_model_3d(Scene3dDisplayable& scene, const Scene3dRequest& request);
  // Reads the "plugins" part into `layers`. Throws std::invalid_argument.
  void read_plugin_displayables(std::span<const std::byte> bytes, Layers& layers) const;
  // The context of set_plugins().
  [[nodiscard]] const PluginContext& plugin_context() const;
  // Reads the "movies" part into `layers`. Throws std::invalid_argument.
  static void read_movies(std::span<const std::byte> bytes, Layers& layers,
                          const std::shared_ptr<std::vector<TextureId>>& retired);

  [[nodiscard]] std::vector<Entry>& entries(Layer layer);
  [[nodiscard]] const std::vector<Entry>& entries(Layer layer) const;

  const Vfs* vfs_;
  std::string language_;
  Size virtual_size_;
  Layers layers_;
  LayerState layer_state_;
  CameraState camera_;
  std::optional<Shake> shake_;
  std::vector<GameShader> shaders_;  // number n is shaders_[n - 1]
  std::array<TextureId, kLayerOrder.size()> layer_targets_{};
  std::vector<ShaderId> retired_shaders_;  // GPU shaders of redeclared shaders, to destroy
  std::map<std::string, Texture, std::less<>> textures_;
  std::shared_ptr<ModelLibrary> models_;  // the models of the 3D scenes
  MovieAudio* movie_audio_ = nullptr;
  const PluginContext* plugins_ = nullptr;
  TextureId plugin_white_{};  // 1 by 1 white, made by prepare() when a plugin displayable is shown
  std::function<void(std::string_view)> warn_;
  std::set<std::string, std::less<>> failed_paths_;  // images that did not load: not tried again
  const VoiceLevel* voice_level_ = nullptr;
  // Textures of movies that went away, for prepare() to destroy.
  std::shared_ptr<std::vector<TextureId>> retired_textures_ =
      std::make_shared<std::vector<TextureId>>();
  std::map<std::string, std::shared_ptr<const LayeredDefinition>, std::less<>> definitions_;
  std::map<std::string, std::shared_ptr<const AnimationDefinition>, std::less<>> animations_;
  Timeline timeline_;
  std::optional<ActiveTransition> transition_;
  TransitionShaders transition_shaders_;
  std::array<TextureId, 2> frames_{};  // the old frame and the new frame, made by prepare()
  IRenderer* renderer_ = nullptr;      // the renderer of prepare()
  mutable OldFrame old_frame_ = OldFrame::kNone;
};

}  // namespace vnmaki
