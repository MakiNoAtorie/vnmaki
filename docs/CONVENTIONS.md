# Engine conventions

These are the rules for contributors and agents. The specification is `docs/SPEC.md`.
Add a rule when later work must obey it. Remove a rule when it is not true.

## Build and test

- Presets: `debug`, `release`, `asan-ubsan`, `tsan`, `fuzz`, `clang-tidy`. They use Ninja and write to `build/<preset>/`.
- `cmake --workflow --preset <name>` configures, builds and runs ctest.
- Plain presets get dependencies from vcpkg (`VCPKG_ROOT`).
- Each preset has a `system-*` version. It finds installed dependencies with `find_package(<pkg> CONFIG REQUIRED)`. Lua uses CMake's FindLua.
- Each dependency must work with the two methods.
- `fuzz` and `clang-tidy` work only on Linux with LLVM Clang. Set `CC=clang CXX=clang++`.
- On Windows, use `tools\dev\vsenv.cmd cmake --workflow --preset debug`. It uses MSVC and the vcpkg in Visual Studio. Sanitizers, clang-tidy and fuzzers then run only in CI.
- In cloud containers, `tools/dev/bootstrap-deps.sh` builds the dependencies for the `system-*` presets.
- While you work, run only the tests for the changed code: `ctest --test-dir build/debug -R <pattern>`.
- Before a push, run `cmake --workflow --preset debug`. CI runs the other presets, clang-tidy and the format check.
- CI (`.github/workflows/ci.yml`) runs:
  - Windows, MSVC: debug, release.
  - Linux, Clang 18: asan-ubsan, tsan, fuzz, release, a build without threads.
  - macOS, Apple Clang: debug, asan-ubsan.
  - A clang-tidy job and a format check.
- CI gets vcpkg at the `builtin-baseline` in `vcpkg.json`.
- If a new vcpkg dependency needs system packages on a runner, add an install step to the workflow. Example: `.github/actions/install-sdl3-deps`.
- vcpkg pins:
  - Lua is 5.4.8, because the specification requires 5.4.
  - `sdl3` has no default features. On Linux, it has `x11` and `wayland`.
  - mpack has no vcpkg port, and `third_party/` has no copy of it. The save format (`engine/saves/save_file.cpp`) has a small MessagePack reader and writer of its own. It is plain MessagePack, so mpack can replace it.
  - zstd is a vcpkg dependency. `engine/saves/CMakeLists.txt` finds `zstd::libzstd` (vcpkg) or the plain library (Debian: `libzstd-dev`).

## Code

- C++20, namespace `vnmaki`.
- RAII. No raw `new` or `delete`; use `std::unique_ptr` by default. Use `std::span` or `std::string_view`, not a pointer and a length.
- Use the Lua C API only through sol2 wrappers.
- Names: types `PascalCase`, functions and variables `snake_case`, enumerators `kName`.
- Scoped enums have a small base type, for example `: std::uint8_t`.
- Files are `.hpp` and `.cpp`. Format: Google style, 100 columns.
- Includes use the path from the repository root, for example `"engine/vfs/vfs.hpp"`.
- Include order: the header of the file, `<std>`, `<third/party.h>`, `"engine/..."`.
- All warnings are errors. clang-tidy uses `WarningsAsErrors: '*'`. Correct the findings. Do not suppress them.
  - Give names to literal constants, also in tests. Integer literals in doctest `CHECK(...)` are permitted.
  - Floating-point literals other than 0 and 1 always need a name.
  - Declare a variable with a cast initializer as `auto`.
  - Give all struct fields default initializers (`PointF position{};`). Designated initializers need this.
  - Call `reserve()` before a `push_back` loop.
- The engine is one static library, `vnmaki_engine`.
  - Each module has `engine/<module>/CMakeLists.txt`. It adds its files with `target_sources(vnmaki_engine PRIVATE ...)`.
  - `engine/CMakeLists.txt` adds each module with `add_subdirectory`.
  - Link third-party libraries PRIVATE. Engine headers do not include them.
  - First-party targets link `vnmaki::options` for warnings, hardening and sanitizers. Do not link it into third-party code.
- Game code reads files only through `Vfs`.
- Only `engine/platform/sdl_*` and the renderer allow-list include SDL. The ctest `lint.no_sdl_outside_platform` does a check of this.
- `project(vnmaki VERSION ...)` sets the version. Code reads it through `vnmaki::engine_version()`.

## Tests

- Tests use doctest. Each doctest executable needs `DOCTEST_CONFIG_USE_STD_HEADERS`.
- Unit tests are in `tests/unit/*_test.cpp`. They run in `vnmaki_unit_tests`. The ctest name of each test is `unit.<name>`.
- Platform tests are in `tests/platform/` and have their own executable. On Linux, they use `SDL_VIDEO_DRIVER=offscreen`.
- CLI tests use `vnmaki_add_cli_test(name ARGS ... EXIT n [STDOUT_LINE t | NO_STDOUT] [STDERR_REGEX r] [EXPECTED_FILES f...])`.
- To compare output with a file, use `vnmaki_add_cli_stdout_file_test(... STDOUT_FILE f)`.
- A `vnmaki run` test never touches the user-data directory (AppData). Both helpers add `--data-dir <build>/tests/cli/data/<name>` to every `ARGS` that starts with `run`, and the runner script empties that folder first. So no test shares saves with another. A test that adds `add_test` by hand must do the same (`vnmaki_isolate_run_data`).
- Do not make test fixtures by hand. A Python 3 script in `tools/generators/` writes them.
  - The script uses only the standard library. Its only argument is `OUT_DIR`.
  - Register it with `vnmaki_add_fixture_generator(<name> SCRIPT ... TEST_TARGETS ...)`.
  - Tests read the fixtures from `VNMAKI_FIXTURES_DIR "/<name>"`.
- Script tests use `vnmaki::test::memory_vfs({{path, text}...})` from `tests/support/memory_vfs.hpp`.
- `RecordingRenderer` (`tests/support/recording_renderer.hpp`) is an `IRenderer` that records the quads of each draw. Use it to test what a displayable draws, without a GPU.
- Fuzz targets use `vnmaki_add_fuzzer(<name> <sources>)`. ctest gives each target a smoke test of 10,000 runs.
  - A fuzz target checks properties of the output, for example idempotence or an independent oracle. If a property is false, it calls `std::abort()`.
  - To check a new fuzz target, add a bug and make sure that the smoke test finds it.
- Threaded tests put a timeout on each wait. If `!JobSystem::threads_supported()`, they return immediately, because jobs run inline.
- GPU tests are in `tests/render/` (`vnmaki_render_tests`).
  - Get a renderer with `vnmaki::test::make_headless_renderer()`.
  - If there is no device, it prints `VNMAKI_SKIP:` and ctest skips the test. `VNMAKI_REQUIRE_GPU=1` stops the skip. Linux CI sets it, because it has lavapipe.
  - Check pixels with `check_golden(read_pixels(target), "<name>")` against `golden/<name>.png`.
  - `tools/generators/render_fixtures.py` makes the golden images with a reference rasterizer.
  - A feature has its own generator, `tools/generators/<area>_fixtures.py`. It imports `Canvas` and the PNG writer from `render_fixtures`. Check with `check_golden_in("<area>", ...)`. Examples: `layered_fixtures.py`, `shader_fixtures.py`, `frame_anim_fixtures.py`.

## Module contracts

### VFS (`engine/vfs/`)

- A stack of `VfsMount`s. A later mount overrides an earlier mount for the same path.
- Functions: `read`, `exists` (files only), `list` (merged and sorted).
- Paths are canonical UTF-8 with `/` separators. They are case-sensitive on all platforms.
- `path.hpp` rejects paths that can go out of the mount or fail on Windows.
- Const methods are thread-safe. `mount()` is not thread-safe.
- A `.vnpak` archive will be a different `VfsMount`.

### Jobs (`engine/jobs/`)

- `JobSystem` returns a `JobHandle<R>` with `ready`, `wait` and `get`.
- Call `get` only one time. It throws the exception of the job again.
- Subsystems get a `JobSystem&`. They do not own a job system.
- A job must not wait on the handle of a different job, because this can cause a deadlock.
- Use `JobHandle`, not `std::future`, for results between threads. `std::future` caused a TSan race.
- `VNMAKI_ENABLE_THREADS=OFF` builds run all jobs inline.

### Platform (`engine/platform/`)

- `Platform` (SDL or headless) changes OS events into `InputEvent`, a `std::variant`.
- `MainLoop` runs a fixed timestep: events, updates, then `draw(alpha)`. You can inject the clock.
- Pointer positions are in window pixels. Convert them with `window_to_virtual(compute_viewport(...), pos)`.
- Clicks in the letterbox bars are outside the virtual area.
- Text input is off when no text field has the focus.
- `user_data_directory(organization, application)` (`sdl_platform.hpp`) is SDL's per-user preference path. Saves go there.
- Only one `SdlPlatform` can exist. Other SDL users call `SDL_InitSubSystem` and `SDL_QuitSubSystem`. They do not call `SDL_Quit`.

### Render (`engine/render/`)

- Engine code draws only through `IRenderer` (`renderer.hpp`).
- `SdlGpuRenderer` is the SDL GPU backend. Make it with `create_for_window(platform)` or `create_headless()`.
- A frame:
  1. `begin_frame`.
  2. One or more passes: `begin_pass({.target, .clear})`, then `draw(quads, DrawState)`.
  3. `end_frame()`. It returns `FrameStats` (`draw_calls`).
- Adjacent quads with the same `DrawState` are one draw call.
- Coordinates are pixels of the pass target. The origin is top-left.
- The GPU uses premultiplied colors. `Image` pixels are premultiplied RGBA8.
- Draw the scene into a render target at virtual resolution.
- Then draw that target to `kScreen` at `compute_viewport(virtual, window_metrics().pixels, mode).rect`.
- Shaders are HLSL in `shaders/`. shadercross compiles them at build time through `vnmaki_add_shaders(...)`.
- Custom fragment shaders include `vnmaki.hlsli`. They can have one constant buffer at `register(b0, space3)`.
- Game shaders compile at run time, not at build time (`shader_compiler.hpp`).
  - `compile_fragment_shader(name, hlsl)` uses the SDL_shadercross library (`sdl3-shadercross` in `vcpkg.json` is a target dependency, not only a host tool). It returns a `CompiledShader` that owns SPIR-V, MSL and DXIL. `bytecode()` is the view for `IRenderer::create_shader`.
  - `#include "vnmaki.hlsli"` finds the engine header. The build embeds `shaders/*.hlsli` as text (`shader_headers.cpp.in`), so a game needs no engine shader files.
  - A compile error throws `ShaderCompileError`. The message has the file, the line, the column and the compiler message. Example: `shaders/sepia.frag.hlsl:5:18: error: use of undeclared identifier 'x'`.
  - A shader may use one texture and one constant buffer at `register(b0, space3)`. Storage textures and buffers are errors.
  - `shader_compiler_available()` is false on macOS, where vcpkg has no `sdl3-shadercross`. Tests that compile shaders return early then.
  - The compiler is only in `engine/render/sdl_shader_compiler.cpp`, because it includes SDL headers.
- `load_image(vfs, jobs, path)` loads an image and returns `JobHandle<Image>`.
- Upload images with `create_texture` on the main thread.

### Text (`engine/text/text.hpp`)

- Functions: `load_font`, `layout_text(font, utf8, pixel_size, wrap_width)`, `revealed_glyphs(count, cps, elapsed)`, `GlyphAtlas::prepare(layout, scale)`, `text_quads(...)`.
- Layout uses virtual pixels. `prepare` rasterizes at `pixel_size * scale`.
- `text_quads` returns quads in screen pixels. Draw text into a target at screen resolution, not at virtual resolution.
- If `prepare` returns true, upload `atlas.image()` again with `create_texture`. Then destroy the old texture.
- The atlas is white and premultiplied by coverage. The quad tint gives the color.
- A `Font` and its atlases belong to one thread. A bad font throws `FontError`.
- Tags: `parse_markup(markup)` (`engine/text/markup.hpp`) returns a `StyledText`; it throws `MarkupError` with the byte offset. `plain_text(text)` makes text without tags. Syntax: `docs/TEXT_TAGS.md`. The runtime checks `say` and `narrate` text and raises a bad tag at the script line.
- Styled layout: `layout_text(FontSet, StyledText, pixel_size, wrap_width)`. `FontSet` has the base family, `font` tag families and the fallback chain (`ProjectConfig::font_fallbacks`). It does bidi (SheenBidi), line breaks (libunibreak) with kinsoku, ruby and links (`link_at`). `TextLayout::fonts` names the fonts of the glyphs, so a `GlyphAtlas` needs no font. `layout_text(font, utf8, ...)` stays for text with one font and no tags.
- `revealed_glyphs(layout, cps, elapsed)` counts the `speed` and `pause` tags. A glyph with a `color` tag has that colour in place of the tint in `text_quads`.
- `GlyphRaster::kDistance` atlases and `sdf_text_shader()` are for outline and shadow text only. Normal text uses `GlyphRaster::kCoverage`.

### Scene (`engine/scene/`)

- `Scene` keeps tagged displayables on the SPEC §9 layers.
- `Scene` implements `SceneState` and `SceneCommands`. Set `ScriptRuntimeOptions::scene` and `::scene_state` to it.
- The runtime includes no scene headers.
- Image name `N` is the VFS file `images/N.png`. If that file does not exist, it is `images/N.jpg`.
- A tag is unique across all layers.
- `show` defaults: layer `characters`, position `center`, anchor bottom-center.
- `DisplayProperties`:
  - Position is in virtual pixels.
  - Anchor and crop are fractions.
  - Rotation is in degrees, clockwise.
- `Scene` keeps `std::unique_ptr<Displayable>`. `find(tag)` returns a `Displayable*`. Use `dynamic_cast` to get an `ImageDisplayable`.
- The snapshot is a map from a part name to bytes: `displayables` (images), `layered`, `animations` and `tweens`. A new feature adds a part. `restore()` ignores a part that it does not know and reads a missing part as empty.
- The snapshot keeps the properties as a replay leaves them. A tween that ends is at its end value. A loop is at the value where it started.
- Tweens write through `Scene::find(tag)->properties()`. `Scene::update(seconds)` moves them. `hide` and `scene` end the tweens of the displayables that they remove.
- `Scene::show` of a tag that is shown does not end its tweens.
- For each frame, call `scene.prepare(renderer, jobs)` on the main thread. Then call `scene.draw(renderer, target)` in a pass on the virtual-resolution target. Give the id of that target: a layer with a shader ends the pass and begins a new pass on `target`.
- Game shaders:
  - `shader(name, { "amount", ... })` declares `shaders/<name>.frag.hlsl` and compiles it when the scripts load. A declaration again with the same uniforms does nothing. The uniforms are floats (at most `kMaxUniforms`, 16) packed in order into `b0` and padded to 16 bytes. `SceneCommands::declare_shader` carries it out and returns the error string.
  - `show(tag, { shader = name, uniforms = { amount = 1 } })` and `layer(name, { shader =, uniforms = })` set them (`SceneCommands::set_shader`). `shader = ""` removes the shader. A `show` without the options keeps them. Uniforms that the call does not name keep their values; a new shader starts with zeros.
  - `DisplayProperties::shader` is the number of the shader (from 1, in declaration order; 0 is the default shader) and `uniforms` are the values. Every displayable draws through `Displayable::draw_state(texture)`, so images and layered sprites get the shader the same way. `Scene::prepare` makes the GPU shaders and gives them to the displayables.
  - A layer with a shader (and displayables) draws into its own render target of the virtual size, and then that target draws with the shader. The layer properties are in `layer_properties(layer)`.
  - The tag `@layer.<name>` is for the uniforms of a layer in `tween`. A `show` tag cannot start with `@layer.`.
  - The snapshot part `shaders` keeps the shader name (not the number) and the 16 uniform values of each displayable and layer. The part is left out when no shader is set. `restore()` ignores a shader that is not declared.
- `SceneCommands` calls return an error string. The prelude raises it at the script line.
- `SceneCommands` calls also run during replay.
- `SceneCommands` has `animation`, `tween`, `finish_tweens` and `tweening` with default no-op bodies, so other implementations do not need an animation system.
- A displayable draws its quads with `draw_state(texture)` (`Displayable`, protected). What a displayable adds to its draws, for example a shader, goes there, so it applies to every kind of displayable.
- `place_corners(properties, size, part)` and `bounding_box(corners)` (`displayable.hpp`) place a box with the properties. Use them in a new displayable.

### Layered sprites (`engine/scene/layered_sprite.*`)

- Script API: `layered(name, { order = { "base", "outfit", "face" }, base = "eileen/base", outfit = { school = "..." }, face = { ... }, default = { outfit = "school", face = "smile" }, crossfade = 0.2 })`. A group is one image (a string) or a table of `value = "image"`. The images are image names, as for `show`. `layered` may be called at any time. Calling it again with the same name replaces the definition.
- `show("eileen", { outfit = "school", face = "smile" })` shows a layered sprite when `image` (or the tag) is a defined name. It changes only the groups that it names, and a sprite that is shown again keeps its other groups. A new sprite has the defaults. A group without a default shows nothing.
- An unknown group, an unknown value, a bad definition or a missing image gives an error at the script line. The prelude checks group names: any `show` option that is not a group and not in `show_options` (in the prelude) is an unknown group. **A new `show` option must be added to `show_options`.**
- `SceneCommands::layered(LayeredRequest)` makes the definition. `ShowRequest::attributes` holds the (group, value) pairs, and `ShowRequest::replaying` is true during a replay.
- The layers draw in the order of `order`, each at its own size from the sprite's top-left corner. The first group is the base. Its image is the size that `anchor`, `scale`, `crop` and `bounds()` measure. A crop shows the part of each layer that it covers.
- A changed group fades its new image in over its old image. The old image stays at full alpha. The fade runs on `Scene::update(seconds)`, the same clock as tweens. A replay, a restore and a definition without `crossfade` cut at once. `finish_tweens` does not end a fade.
- The snapshot part `layered` has the definition name, the layer position, the properties and the (group, value) of each sprite. It has no definition. `restore()` throws if the scene does not know the definition, so the scripts must load first. A fade that runs is saved as its end.
- `Scene::prepare` loads the images that a sprite shows now. `LayeredSprite::missing_paths()` and `set_texture(path, ...)` do this.

### Frame animation (`engine/scene/frame_animation.*`)

- Script API: `animation(name, { frames = { "a", "b" }, fps = 12, loop = true })` or `animation(name, { sheet = "x", columns = 4, rows = 2, count = 7, fps = 12, loop = true })`. The images are image names, as for `show`. `fps` is required, from above 0 to 1000. `loop` is true by default. A sheet is read row by row. `count` is the number of cells that are frames (all by default). There are at most 4096 frames. A bad definition or a missing image gives an error at the script line.
- `SceneCommands::animation(AnimationRequest)` makes the definition. `show(tag, { image = name })` (or `show(name)`) shows it, as for an image. A layered sprite takes the place of an animation, and an animation takes the place of an image of the same name. Define an animation before the layered sprite that uses it.
- A `FrameAnimation` is a `Displayable`. It draws one frame through `draw_state`: a whole image, or a cell of a sheet (the uv of the frame, cropped by `crop`). The frame size is the image size, or the cell size. `Scene::prepare` loads all the images of an animation (`missing_paths()` and `set_texture(path, ...)`).
- `Scene::update(seconds)` is its only clock. The frame is `floor(time * fps)`, and it wraps when `loop` is true. If `loop` is false, the last frame stays. `AnimationDefinition::frame_at` has the rule.
- A value of a layered group can be an animation: `eyes = { open = "eyes_open", blink = "blink" }`. It plays from frame 1 when the value is chosen, and a group that keeps its value keeps playing. In a crossfade, the old value shows the frame that it had when it left.
- `wait(tag)` and `wait_all()` wait for an animation that does not loop: `Scene::tweening(tag)` is true until its last frame has played for its full time. A loop never blocks a wait. `finish_tweens` (a click, skip mode) puts the animation at its last frame. The same is true for the animation values of a layered sprite (`LayeredSprite::animating()` and `finish_animations()`).
- The snapshot part `animations` has the layer position, the tag, the definition name and the properties. It has no definition and no time. `restore()` throws if the scene does not know the definition, so the scripts must load first. After a restore (a load, a rollback), an animation restarts at frame 1.

### Anim (`engine/anim/`)

- `easing.hpp`: `Ease` is linear, and sine, quad, cubic, back and bounce, each as `in`, `out` and `in_out`. The script names are `linear` and `in_sine`, `out_quad`, `in_out_back`, ... `apply_ease` clamps its input to [0, 1].
- `Timeline` runs `TweenNode` trees: a tween, a `sequence` or a `parallel` group. It has no clock. The caller gives the elapsed seconds to `update()`. Tests use a fake clock.
- A `Timeline::Lookup` finds the `DisplayProperties` of a tag. The timeline keeps no pointers to displayables. A tween of a tag that is not found does nothing.
- A tween changes channels (`Channel`: position, anchor, scale, rotation, alpha, tint, crop). The 16 shader uniforms are channels after them (`uniform_slot(i)`), so `kChannelCount` is 32. This changed the snapshot form of loops. A channel without a target stays as it is. Alpha and tint are kept in [0, 1], also for a back easing.
- A tween starts after its `delay` and reads its start values then. A sequence of tweens on one property continues from the last end. The values do not depend on the step size.
- `yoyo` goes to the target and back. Without `loop`, it does this one time. `loop` repeats a tween forever. On the outermost group, `loop` repeats the whole group. The `delay` of a group applies one time.
- A loop in a sequence must be the last item. A nested group cannot loop. `start()` returns the reason.
- Loops never block a `wait`. `busy(tag)` is true only for tweens that end.
- `start(node, lookup, instant = true)` is for replay. A tween that ends goes to its end value. A loop starts at time 0.
- `finish(tag)` puts the tweens that end at their end values. A click or skip does this.
- `settle()`, `capture()` and `restore()` are the snapshot. `capture()` keeps only loops, with the whole group if it contains a loop. `restore()` starts them at time 0 from the properties that it finds. `Scene::capture()` calls `settle()` on copies first.
- Script API: `tween(tag, { x, y, anchor = { x, y }, scale = number or { x, y }, rotation, alpha, tint = "#rrggbb", crop = { x, y, width, height }, duration, delay, ease, loop, yoyo, uniforms = { name = number } })`. The units are those of `show`. `duration` is 0.5 s if the script gives none.
- `sequence { item, ... }` and `parallel { item, ... }` start a group at once. An item is `{ tag, <options of tween> }`, or a group: `{ sequence = { items } }` or `{ parallel = { items } }`. The group table can have `delay` and `loop`. A group nests up to 16 levels.
- `tween` has an effect at once and does not block. `wait(tag)` and `wait_all()` block (`WaitRequest`).
- A wait is always one step, also when nothing is tweening, so replay counts the same steps as live play. `WaitPresenter` (`wait_presenter.hpp`) ends it:
  - Call `update()` once per update after `Scene::update`. It ends a wait whose tweens have ended.
  - Call `skip()` on a click or in skip mode, before `DialoguePresenter::advance()`. It puts the tweens at their end values and goes on. If it returns true, the input is used.
- `ScriptPosition::wait` marks a wait step. Rollback goes back over wait steps to the previous say, narrate, menu or `call_screen` (a wait is never a rollback step).

### Dialogue (`engine/dialogue/`)

- `DialoguePresenter` keeps the current say or narrate line and its reveal.
- Make it after `runtime.start()`. Then advance the story only through `presenter.advance()`.
- The first advance completes a reveal. The next advance goes to the next line.
- `SayBoxView`: call `prepare(line, scale)` before `begin_frame`.
- Then call `draw(...)` in a screen-resolution pass. Use the viewport scale and the top-left of the viewport as the origin.
- `SayBoxView` lays out the line with `FontSet` (tags, bidi, fallback; the speaker name is plain text). Pass `SayBoxView::revealed` as the presenter's `Reveal`, so `speed` and `pause` work. `link_at(line, virtual_point)` gives the `link` id; `vnmaki run` passes it to `runtime.activate_link`. The window loads the fallback chain of the first language in `project.toml`.
- `Playback` (`playback.hpp`) does skip and auto. Make it after the presenter. Each frame call `presenter.update(dt)`, then `playback.update(dt)`.
  - Skip `kSeen` goes on while the line is in the seen set. It stops at the first unseen line, and at any menu or screen. `kAll` stops at menus and screens only. `PlaybackOptions::skip_wait` ends a `wait` for tweens (`WaitPresenter::skip`) and skip goes on.
  - Auto waits `auto_base_delay + auto_seconds_per_glyph × glyphs × auto_multiplier` after the line is fully shown. While `PlaybackOptions::voice_playing` returns true it waits, and after a line that had a voice it waits only `auto_base_delay`.
  - `Playback` marks each line it sees in the `PersistentStore`. It takes the script's `skip` and `auto` calls. `vnmaki run`: `A` toggles auto, `S` cycles skip (off, seen, all), and an advance input turns skip off.
  - `DialoguePresenter::line_serial()` changes when line() shows another line. The presenter also follows a story that moved by itself (a load).
- `PlaybackIndicator` (`engine/ui/playback_indicator.hpp`) shows the screen `vnmaki:playback` ("SKIP", "SKIP ALL", "AUTO", ...) while a mode is on. Call `sync(skip, automatic)` every frame.
- The default font of a project is `fonts/default.ttf` in its VFS. The engine has no font.
- `vnmaki run <dir> [--frames N] [--data-dir <path>]` plays in a window. It exits with 0 when the story ends. Saves and `persistent.vnpersist` go to `--data-dir`, or else to the user-data directory of the project title. This also applies to `--headless`.
- Rollback input (`is_rollback_event`, `rollback_input.hpp`): mouse wheel up, Page Up, gamepad left shoulder. The story window checks it after the screens and calls `ScriptRuntime::roll_back()`. It turns skip off. Task 42 will make the keys configurable.
- `SayRequest` has `color` (0xRRGGBB) and `voice_tag` from `character{}`.
- `Backlog` (`backlog.hpp`) has no list of its own. `entries()` reads `ScriptRuntime::rollback_steps()`, so it is right after a rollback and after a load. It is the say and narrate steps (menus and screens are left out), oldest first, at most `max_entries` of the newest (`project.toml` `backlog_length`, default 100). It cannot hold more than `rollback_depth` steps plus one.
  - A `BacklogEntry` has `speaker` (empty for narrate), `text` (with tags), `label`, `step` and `voice`. `voice` is the name of the last `voice(name)` call before the line (`RollbackStep::voice`). The runtime records it also during a replay, but does not play it.
  - `play_voice(entry)` calls `AudioCommands::voice`, so a replayed voice also ducks the music and `voice_playing()` is true. Give `Backlog` the same `Audio` as the runtime.
- `BacklogScreen` (`engine/ui/backlog_screen.hpp`) is the screen `vnmaki:backlog` of a `UiHost` (not modal). The story window gives it every event first. While it is open it takes all key, wheel, gamepad button and pointer events, and the story window does not call `Playback::update`, so auto and skip wait.
  - `L` or the gamepad north button opens it (not while a modal screen is shown). `Esc`, `L` or the east button closes it.
  - Up/Down, the d-pad and the wheel select the previous or next line. Page Up/Down and the shoulders move a page. Home/End go to the oldest or newest line. Enter, Space, the south button or a click on a line plays its voice (only a line with a voice is a button).
  - The scroll viewport is a window of whole entries: `UiRenderer` has no clipping. `BacklogScreen` measures each entry with the `TextMeasure` it gets and shows the entries that fit from `first_visible()`. A line taller than the canvas overflows.

### UI (`engine/ui/`)

- A screen is a `UiNode` tree with a name. Kinds: `box`, `hbox`, `vbox`, `text`, `image`, `button`. The Lua constructors have the same names.
- Lua: `screen "name" { nodes }`, or `screen("name", function(args) return nodes end)`. A table of several nodes is wrapped in a `box`. `show_screen`, `hide_screen` and `call_screen` take the screen name and an `args` table for the function form.
- `build_ui_tree` documents the options (`width`, `padding`, `bg`, `returns`, ...). An unknown option is an error that names the node.
- A text or button string is the first string in the table: `button { "Save", returns = "save" }`. A button hands `returns` to its screen when activated.
- A screen is built when it is shown. It does not update when `store` changes. `bind` is a later task.
- Yoga does the layout (`layout_tree`). Only `ui_layout.cpp` includes it, and it is linked PRIVATE.
  - The root fills the canvas. Lengths are virtual pixels. A node with `x` or `y` is positioned absolutely in its parent.
  - Give `layout_tree` a `TextMeasure`. `UiRenderer::text_measure()` measures with the fonts. `estimate_text_size` is for tests.
- Each node has a `role`, a `label` (default: its text) and a `state` (`focused`, `hovered`, `disabled`). These are the data for the accessibility tree.
- Only enabled buttons take the focus. Tab order is document order. The arrow keys and the d-pad pick the nearest button in that direction. Pointer hover moves the focus.
- `UiHost` is the stack of screens (the last one shown is on top).
  - A screen with the same name is replaced. `handle_event(event, viewport)` returns `consumed` and an optional `ScreenActivation`.
  - A modal screen takes the keys, the d-pad and all clicks. Other screens take the pointer only.
  - `UiHost` does not store screens in snapshots. `show_screen` and `hide_screen` run again during replay, so they must be idempotent.
- `ScreenPresenter` shows the modal screen of a `menu` or `call_screen` request and answers the runtime. Make it after `runtime.start()`. Call `sync()` after anything else that moves the story. Give it events before the dialogue presenter: if `handle_event` returns true, do not advance.
- `UiRenderer` draws trees through `IRenderer`. Call `prepare(root, scale)` before `begin_frame`. Draw into a screen-resolution pass with the viewport scale and origin, like `SayBoxView`.
  - It has one glyph atlas for each text size. A node with `markup = true` uses text tags.
  - An image node draws `images/<name>.png` (else `.jpg`) from the VFS. It needs `width` and `height`.
- The say box is a screen: `make_say_screen` builds it, and `SayBoxView` lays it out and draws it with a `UiRenderer`. A Lua replacement is task 18.

### Audio (`engine/audio/`)

- `Audio` is the mixer. It implements `SceneState` and `AudioCommands`.
- Set `ScriptRuntimeOptions::audio` to it. Also put it in `scene_state`, inside a `CompositeSceneState` when there is a `Scene`.
- The runtime includes no audio headers.
- Audio name `N` is the VFS file `audio/N.opus`, else `.ogg`, `.flac` or `.wav`. `voice(N)` plays `voice/N`.
- Channels: `music`, `sound`, `voice`, `ambient`, `ui`. A script makes another by naming it.
- `AudioOutput::kManual` has no device. `update(seconds)` mixes that much. Tests and headless runs use it.
- `kNullDevice` mixes on a device thread to nowhere. `kDevice` is the system output.
- Call the commands, `update()` and `restore()` from one thread.
- `vnmaki run` in a window makes an `Audio` with the system output (the null device if there is none) and calls `update()` every frame. A headless run uses `kManual` and never mixes.
- Music crossfades. The same music on a channel carries on. The music channel ducks while a voice plays.
- The snapshot has each channel's volume and music. `restore()` stops sounds and voices.
- Music tracks change at the next `update()`. A replay then gives the same music, so it does not stop.
- The runtime does not pass `sound` and `voice` on during replay.
- `voice_playing()` is true while a voice plays. `Playback` (Dialogue) waits for it: set `PlaybackOptions::voice_playing`. `voice(name)` also replays a backlog voice.
- `AudioCommands` calls return an error string. The prelude raises it at the script line.
- Decoders are in `audio_decoder.hpp`. A bad file throws `AudioError`.
- Fuzz targets: `fuzz_audio` (decoders) and `fuzz_audio_state` (snapshot).

### Runtime (`engine/runtime/`)

- `ScriptRuntime` loads all `.lua` files in `scripts/` through the VFS. Each label runs as a coroutine.
- The script API is a Lua prelude in `script_runtime.cpp`. Private `engine.*` primitives do the work.
- All blocking calls go through `Impl::yield_at`.
  - It counts the step and calls `ScriptObserver::on_blocked`.
  - Steps start at 1 for each label activation.
- `jump`, `call` and `ret` are transfers, not steps.
- The sandbox has no `io`, `debug`, `package` or `require`. `vfs.*` is the only file access.
- `store` is the only game state.
  - It keeps booleans, numbers, strings and tables with scalar keys.
  - A different value stops the story at the next label entry.
  - `rand` is `math.random`. Its state is `store.__rng`.
- `ScriptRuntime::replaying()` is the only replay flag.
  - Subsystems update their state during replay.
  - They check the flag only for effects that the player can see or hear.
- `replay(LabelSnapshot, inputs, step)` restores the label-entry snapshot and replays the top label only.
  - It does not make the callers again.
  - Only menu choices and `call_screen` results go into the input log.
- Scene state goes into snapshots through `SceneState` (`capture()` and `restore(bytes)`) in `ScriptRuntimeOptions::scene_state`.
- `menu { "a", "b" }` returns a 1-based number. C++ gives the answer with `choose(index)`, which is 0-based. `ScreenPresenter` (`engine/ui/`) shows the menu and calls `choose`.
- `call_screen(name, args)` is a blocking call (`CallScreenRequest`). C++ gives the value with `return_screen(value)`. The value is nil, a boolean, a number or a string.
- `ScriptRuntimeOptions::screens` (`ScreenCommands`, implemented by `UiHost`) carries out `show_screen` and `hide_screen`.
  - `check_screen` runs on every tree first. Its error string is raised at the script line.
  - The runtime includes no ui headers. It reads the Lua node tables into `ScreenNodeDef`.
- `label_source_hash(name)` covers the lines of the label function. It does not cover the helpers that it calls.
- `wait(tag)` and `wait_all()` are blocking calls too (`WaitRequest`). `advance()` ends them. See Anim.
- The headless runner prints a transcript (`label X`, `say Who: text`, ...). `vnmaki run <dir> --headless` runs it. It ends a wait at once and needs no input for it.
- `project.toml` has `title` (required), `version`, `resolution`, `languages`, `auto_save` (a list of `"label"` and `"menu"`), `rollback_depth` and `backlog_length` (each 0 to 10000, default 100).
- `save(slot)`, `load(slot)`, `quick_save()` and `quick_load()` only queue a `SaveRequest`. `take_save_requests()` gives them to the host. They are dropped during a replay.
  - A slot is an integer from 1. `load` with a number is a slot. With a string or function it is Lua's `load`.
- `on_load(function(old_version, store) ... end)` registers the migration hook while the scripts load. `run_on_load(snapshot, old_version)` runs it on the store of a snapshot, not on the live `store`.
- `persistent` is a second Lua table, like `store`, but it is not in any snapshot or save. Rollback and load never change it.
  - `persistent_snapshot()` and `restore_persistent()` give the host the data. Call `restore_persistent` before `load_scripts()`.
  - A replay runs script code again, so code that writes to `persistent` must be idempotent (`persistent.won = true`, not `persistent.n = persistent.n + 1`).
- Rollback keeps its history in the runtime. `roll_back()` and `rollback_steps()` are the interface.
  - A step (`RollbackStep`) is a position where the story waited at a say, narrate, menu or `call_screen`. A `wait` is not a step. It holds the label-entry snapshot and input log of the outermost running label, the number (`ordinal`) of the yield point, and `voice`, the last `voice(name)` before it (see Backlog in Dialogue). So each step stands alone, and the history works across `jump`, `call` and `ret`.
  - `roll_back()` replays the outermost label from its snapshot, with the labels it called, to that yield point (`begin_replay` with an ordinal). Replay does not report the label entries, and it does not add a step. The observer then hears the request, as for a load.
  - From a say or menu it goes to the step before. From a wait it goes to the last step. It returns false when there is nothing to go back to, and then it changes nothing.
  - The history keeps `ScriptRuntimeOptions::rollback_depth` steps back (default 100, `project.toml` key `rollback_depth`, 0 is off). The oldest steps go first.
  - A rollback drops the later steps at once. There is no roll forward. The player who goes back to a menu and chooses again gets a new branch with no trace of the old one.
  - `menu { "a", "b", fixed = true }`: after the player answers it, the history is empty. Rollback cannot go back to the menu or before it. `fixed` must be a boolean.
  - `replay()` (a load) empties the history. The story then starts a new one.
  - Rollback and `persistent`: rollback never changes it. A replay runs script code again, so the idempotence rule for `persistent` is also true here.
  - `show_screen` and `hide_screen` are not in snapshots (see UI), so a rollback does not undo them.
  - The headless runner has `RollbackInput`. It fails the run when there is nothing to go back to.
- `skip("seen" | "all" | "off" | false)` and `auto(bool)` queue a `PlaybackRequest`. `take_playback_requests()` gives them to the host. They are dropped during a replay.
- `character { auto_multiplier = n }` goes to `SayRequest::auto_multiplier` (default 1).
- `drop_callers()` makes the running label the outermost label. A load calls it before `replay()`.

### Saves (`engine/saves/`)

- A save has these fields: versions, label, step, source hash, label-entry snapshot, input log, persistent pointer, thumbnail, time, title. `save_file.hpp` has the byte layout.
  - Format: zstd around MessagePack. A map with string keys. A reader skips keys that it does not know. A file with another `format` number is rejected.
  - The persistent pointer is a string (`SavesOptions::persistent`). The persistent store itself is not in the save.
- Saves are files in `SavesOptions::directory`, one for each slot: `<slot>.vnsave`. They are not in the VFS.
  - The host gives the directory: `user_data_directory(...)` in a game, a temporary directory in a test.
  - A slot name is 1 to 32 characters of `a-z`, `0-9`, `_` and `-`. Scripts use numbers, `quick` and `auto`.
  - A file is written beside its slot and renamed over it.
- `decode_save` never trusts a file. It rejects bad data with `SaveError`.
  - It also checks what replay needs: table references, NaN keys, the thumbnail size, the payload size (`kMaxSavePayloadBytes`).
  - `fuzz_save_loader` checks this. It edits a valid payload, so it has no seed corpus.
- `Saves` works with one `ScriptRuntime`. `save()` needs `RunState::kWaiting`.
  - It saves the running label only. A save made inside a called label has no callers, so `ret` at the end of that label ends the story.
  - `poll()` is for the host. Call it whenever the story waits. It does the requests of scripts, then the auto save.
  - A script request is done at the next yield point after the call, not at the call.
  - Auto-save events: `label` (step 1 of a label) and `menu`. Each position fires once. A load does not auto save its own position.
  - Errors of requests that have no caller (scripts, auto save) go to `SavesOptions::on_warning`.
- `load(slot)` does these steps:
  1. Decode the file. A bad file, a missing label or a failed `on_load` returns an error. The story is not changed.
  2. Run `on_load(old_version, store)` on the saved store. `old_version` is the game version in the save. It runs on every load.
  3. If `label_source_hash` is different, call `on_warning` and `replay(snapshot, {}, 0)`: the label starts from its first line.
  4. Otherwise `replay(snapshot, inputs, step)`.
  5. If the replay fails (for example, a helper that the label calls was changed), replay the earlier position. Return the error.
- Thumbnails are RGBA8 with premultiplied alpha, at most `kThumbnailWidth` wide (`thumbnail.hpp`).
  - `capture_thumbnail(renderer, target)` reads the target with `read_pixels`. Call it between frames. Set it as `SavesOptions::thumbnail`.
  - Without a thumbnail function, saves get `stub_thumbnail()`. Headless runs and tests use it.
- This module has no UI. Task 18 shows the slot list (`Saves::list()`).
- `PersistentStore` (`persistent_store.hpp`) keeps `persistent` and the seen steps in `<directory>/persistent.vnpersist`, in the same directory as the saves.
  - The file is plain MessagePack (`encode_persistent`, `decode_persistent` in `save_file.hpp`). A bad file is reported through `on_warning`, and the game starts with no data.
  - `load()` runs before `load_scripts()`. `flush()` writes the file only if `persistent` or the seen set changed. Call it whenever the story waits. The destructor calls it too.
  - A seen step is `{label, step, source hash of the label}` (`SeenStep`). An edited label has a new hash, so all its steps are unseen.
- Hosts call `Saves::poll()` and `PersistentStore::flush()` once for each frame (`vnmaki run`) or after each input (`run_headless` with `HeadlessOptions::saves`).
