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
- Before a push, run `debug`, `asan-ubsan` and `clang-tidy`. Also run `tsan` for threaded code and `fuzz` for loaders.
- Then run `git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror`.
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
  - mpack has no vcpkg port. It is in `third_party/mpack/` (MIT).

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
- CLI tests use `vnmaki_add_cli_test(name ARGS ... EXIT n [STDOUT_LINE t | NO_STDOUT] [STDERR_REGEX r])`.
- To compare output with a file, use `vnmaki_add_cli_stdout_file_test(... STDOUT_FILE f)`.
- Do not make test fixtures by hand. A Python 3 script in `tools/generators/` writes them.
  - The script uses only the standard library. Its only argument is `OUT_DIR`.
  - Register it with `vnmaki_add_fixture_generator(<name> SCRIPT ... TEST_TARGETS ...)`.
  - Tests read the fixtures from `VNMAKI_FIXTURES_DIR "/<name>"`.
- Script tests use `vnmaki::test::memory_vfs({{path, text}...})` from `tests/support/memory_vfs.hpp`.
- Fuzz targets use `vnmaki_add_fuzzer(<name> <sources>)`. ctest gives each target a smoke test of 10,000 runs.
  - A fuzz target checks properties of the output, for example idempotence or an independent oracle. If a property is false, it calls `std::abort()`.
  - To check a new fuzz target, add a bug and make sure that the smoke test finds it.
- Threaded tests put a timeout on each wait. If `!JobSystem::threads_supported()`, they return immediately, because jobs run inline.
- GPU tests are in `tests/render/` (`vnmaki_render_tests`).
  - Get a renderer with `vnmaki::test::make_headless_renderer()`.
  - If there is no device, it prints `VNMAKI_SKIP:` and ctest skips the test. `VNMAKI_REQUIRE_GPU=1` stops the skip. Linux CI sets it, because it has lavapipe.
  - Check pixels with `check_golden(read_pixels(target), "<name>")` against `golden/<name>.png`.
  - `tools/generators/render_fixtures.py` makes the golden images with a reference rasterizer.

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
- `load_image(vfs, jobs, path)` loads an image and returns `JobHandle<Image>`.
- Upload images with `create_texture` on the main thread.

### Text (`engine/text/text.hpp`)

- Functions: `load_font`, `layout_text(font, utf8, pixel_size, wrap_width)`, `revealed_glyphs(count, cps, elapsed)`, `GlyphAtlas::prepare(layout, scale)`, `text_quads(...)`.
- Layout uses virtual pixels. `prepare` rasterizes at `pixel_size * scale`.
- `text_quads` returns quads in screen pixels. Draw text into a target at screen resolution, not at virtual resolution.
- If `prepare` returns true, upload `atlas.image()` again with `create_texture`. Then destroy the old texture.
- The atlas is white and premultiplied by coverage. The quad tint gives the color.
- A `Font` and its atlases belong to one thread. A bad font throws `FontError`.
- This version has one left-to-right run for each paragraph and one font. It has no tags, bidi or font fallback.

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
- Tweens write through `Scene::find(tag)->properties()`.
- For each frame, call `scene.prepare(renderer, jobs)` on the main thread. Then call `scene.draw(renderer)` in a pass on the virtual-resolution target.
- `SceneCommands` calls return an error string. The prelude raises it at the script line.
- `SceneCommands` calls also run during replay.

### Dialogue (`engine/dialogue/`)

- `DialoguePresenter` keeps the current say or narrate line and its reveal.
- Make it after `runtime.start()`. Then advance the story only through `presenter.advance()`.
- The first advance completes a reveal. The next advance goes to the next line.
- `SayBoxView`: call `prepare(line, scale)` before `begin_frame`.
- Then call `draw(...)` in a screen-resolution pass. Use the viewport scale and the top-left of the viewport as the origin.
- The default font of a project is `fonts/default.ttf` in its VFS. The engine has no font.
- `vnmaki run <dir> [--frames N]` plays in a window. It exits with 0 when the story ends.
- `SayRequest` has `color` (0xRRGGBB) and `voice_tag` from `character{}`.

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
  - Only menu choices go into the input log.
- Scene state goes into snapshots through `SceneState` (`capture()` and `restore(bytes)`) in `ScriptRuntimeOptions::scene_state`.
- `menu { "a", "b" }` returns a 1-based number. C++ gives the answer with `choose(index)`, which is 0-based. There is no menu UI.
- `label_source_hash(name)` covers the lines of the label function. It does not cover the helpers that it calls.
- The headless runner prints a transcript (`label X`, `say Who: text`, ...). `vnmaki run <dir> --headless` runs it.
- `project.toml` has `title` (required), `version`, `resolution` and `languages`.
