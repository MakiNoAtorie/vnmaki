# Engine conventions

These are the rules for contributors and agents. The specification is `docs/SPEC.md`.
Add a rule when later work must obey it. Remove a rule when it is not true.

## Build and test

- Presets: `debug`, `release`, `asan-ubsan`, `tsan`, `fuzz`, `clang-tidy`. They use Ninja and write to `build/<preset>/`.
- The option `VNMAKI_DEV_TOOLS` builds `engine/tools/` and links Dear ImGui. It is ON for builds with a debug type and OFF for `Release`, `MinSizeRel` and `RelWithDebInfo`. It sets `VNMAKI_DEV_TOOLS` to 1 or 0 in the engine's public compile definitions, so code that uses the tools has `#if VNMAKI_DEV_TOOLS`. A release build compiles no tool code, and its tests do not have the tool tests.
- The option `VNMAKI_SHADER_COMPILER` links SDL_shadercross, the run-time compiler of game shaders (see Render). It is ON for builds with a debug type and OFF for `Release`, `MinSizeRel` and `RelWithDebInfo`. It sets `VNMAKI_RUNTIME_SHADERS` for `engine/render/sdl_shader_compiler.cpp`. A release build links no shader compiler: its game shaders are the `.vnshader` files that `vnmaki import` made (see Asset import), and `vnmaki import` cannot compile a shader there. `shader_compiler_available()` is false in such a build.
- `cmake --workflow --preset <name>` configures, builds and runs ctest.
- Plain presets get dependencies from vcpkg (`VCPKG_ROOT`).
- Each preset has a `system-*` version. It finds installed dependencies with `find_package(<pkg> CONFIG REQUIRED)`. Lua uses CMake's FindLua.
- Each dependency must work with the two methods.
- `fuzz` and `clang-tidy` work only on Linux with LLVM Clang. Set `CC=clang CXX=clang++`.
- On Windows, use `tools\dev\vsenv.cmd cmake --workflow --preset debug`. It uses MSVC and the vcpkg in Visual Studio. Sanitizers, clang-tidy and fuzzers then run only in CI.
- In cloud containers, `tools/dev/bootstrap-deps.sh` builds the dependencies for the `system-*` presets.
- While you work, run only the tests for the changed code: `ctest --test-dir build/debug -R <pattern>`.
- Before a push, run `tools\dev\check.cmd` (the debug and release workflows). CI runs the other presets, clang-tidy and the format check.
- CI (`.github/workflows/ci.yml`) runs:
  - Windows, MSVC: debug (with `/analyze`), release.
  - Linux, Clang 18: asan-ubsan, tsan, fuzz, release, a build without threads.
  - macOS, Apple Clang: debug, asan-ubsan.
  - A clang-tidy job and a format check.
- CI gets vcpkg at the `builtin-baseline` in `vcpkg.json`.
- If a new vcpkg dependency needs system packages on a runner, add an install step to the workflow. Example: `.github/actions/install-sdl3-deps`.
- vcpkg pins:
  - Lua is 5.4.8, because the specification requires 5.4.
  - `sdl3` has no default features. On Linux, it has `x11` and `wayland`.
  - mpack has no vcpkg port, and `third_party/` has no copy of it. The save format (`engine/saves/save_file.cpp`) has a small MessagePack reader and writer of its own. It is plain MessagePack, so mpack can replace it.
  - libwebp is a vcpkg dependency. `engine/render/CMakeLists.txt` finds it with `find_package(WebP CONFIG REQUIRED)` (target `WebP::webp`). Debian: `libwebp-dev`.
  - dav1d (AV1) and libwebm (WebM, its `mkvparser`) are vcpkg dependencies. Neither has a CMake config that the engine uses, so `engine/video/CMakeLists.txt` finds both with `find_path` and `find_library` (libwebm's headers are in `include/webm`).
  - dav1d 1.5.4 does not build with MSVC 14.5 (Visual Studio 2026): its `stdatomic.h` shim uses `__has_include_next` in a non-clang build. `vcpkg-configuration.json` adds the overlay port `tools/vcpkg-overlay/dav1d`, which is the vcpkg port with one patch for this. Remove the overlay when the vcpkg baseline has a dav1d that builds.
  - Dear ImGui (`imgui`, feature `sdlgpu3-binding`) is a vcpkg dependency of the dev tools. `engine/tools/CMakeLists.txt` finds it with `find_package(imgui CONFIG REQUIRED)` (target `imgui::imgui`). `tools/dev/bootstrap-deps.sh` does not build it yet, so a `system-*` preset needs an installed ImGui with CMake config files, or `-DVNMAKI_DEV_TOOLS=OFF`.
  - efsw (file watching for hot reload) is a vcpkg dependency of the dev tools. `engine/tools/CMakeLists.txt` finds it with `find_package(efsw CONFIG REQUIRED)` (target `efsw::efsw`, a DLL on Windows). Only `engine/tools/file_watcher.cpp` includes it. A release build does not link it. `tools/dev/bootstrap-deps.sh` does not build it yet, so a `system-*` preset needs an installed efsw with CMake config files, or `-DVNMAKI_DEV_TOOLS=OFF`.
  - libopusenc (WAV and FLAC to Opus), cgltf (glTF checks) and HarfBuzz's subsetter (font subsets) are vcpkg dependencies of `engine/assets/`. libopusenc has no CMake config, so `engine/assets/CMakeLists.txt` finds `opusenc.h` (in `include/opus`) and the library with `find_path` and `find_library` (Debian: `libopusenc-dev`). cgltf is one header (`cgltf.h`). Only `engine/assets/cgltf_impl.cpp` defines `CGLTF_IMPLEMENTATION`, so other code that uses cgltf includes `<cgltf.h>` and does not define it. vcpkg's `harfbuzz` has the target `harfbuzz::harfbuzz-subset` (`hb-subset.h`); without it, the plain library `harfbuzz-subset` is used.
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
  - MSVC `/analyze` runs in the Windows debug CI job. Locally: `tools\dev\vsenv.cmd cmake --preset debug -B build/analyze -DVNMAKI_MSVC_ANALYZE=ON`, then build in `build/analyze`. The option adds `/analyze /analyze:external-` to `vnmaki::options` only, so third-party headers (SYSTEM includes) are not analyzed. Its warnings are errors. Correct them. Do not suppress them. Put a dependency's headers on a SYSTEM include path (imported targets do this).
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
- Only `engine/platform/sdl_*`, `engine/render/sdl_*` and `engine/tools/imgui_backend.cpp` include SDL. The ctest `lint.no_sdl_outside_platform` does a check of this.
- `project(vnmaki VERSION ...)` sets the version. Code reads it through `vnmaki::engine_version()`.

## Tests

- Tests use doctest. Each doctest executable needs `DOCTEST_CONFIG_USE_STD_HEADERS`.
- Every `main` calls `vnmaki::disable_crt_dialogs()` (`engine/core/no_crt_dialogs.hpp`) first. MSVC debug builds otherwise open a modal dialog on `abort()` or an assert, which blocks the run on the desktop. This also applies to throwaway programs. Run them with a timeout.
- Unit tests are in `tests/unit/*_test.cpp`. They run in `vnmaki_unit_tests`. The ctest name of each test is `unit.<name>`.
- Platform tests are in `tests/platform/` and have their own executable. On Linux, they use `SDL_VIDEO_DRIVER=offscreen`. `file_watcher_test.cpp` (efsw on a real folder, Windows only) writes real files and polls with a 10 second timeout, so a watcher that reports nothing fails the test and does not hang it.
- CLI tests use `vnmaki_add_cli_test(name ARGS ... EXIT n [STDOUT_LINE t | NO_STDOUT] [STDERR_REGEX r] [EXPECTED_FILES f...])`.
- To compare output with a file, use `vnmaki_add_cli_stdout_file_test(... STDOUT_FILE f)`.
- A `vnmaki run` test never touches the user-data directory (AppData). Both helpers add `--data-dir <build>/tests/cli/data/<name>` to every `ARGS` that starts with `run`, and the runner script empties that folder first. So no test shares saves with another. A test that adds `add_test` by hand must do the same (`vnmaki_isolate_run_data`).
- Do not make test fixtures by hand. A Python 3 script in `tools/generators/` writes them.
  - The script uses only the standard library. Its only argument is `OUT_DIR`.
  - Register it with `vnmaki_add_fixture_generator(<name> SCRIPT ... TEST_TARGETS ...)`.
  - Tests read the fixtures from `VNMAKI_FIXTURES_DIR "/<name>"`.
- One binary fixture is committed: `tests/fixtures/video/clip.webm`. Python cannot write AV1, so the clip is not generated. It is a small CC0 clip of solid-color frames and a tone, and `tests/fixtures/video/README.md` has the exact ffmpeg command that made it. Do not commit another binary fixture without the owner's approval. The owner approved one more copy of it: `templates/default/movies/clip.webm` (a test checks that the two files are equal).
- Script tests use `vnmaki::test::memory_vfs({{path, text}...})` from `tests/support/memory_vfs.hpp`.
- `RecordingRenderer` (`tests/support/recording_renderer.hpp`) is an `IRenderer` that records the quads of each draw. Use it to test what a displayable draws, without a GPU.
- A test that writes to the system temp directory takes its path from `vnmaki::test::unique_temp_path(prefix, name)` (`tests/support/temp_dir.hpp`). It has a random part for each process, because several ctest runs (other worktrees too) can run the same test at once. The `TempDir` classes of `new_project_test`, `saves_test`, `playback_test` and `pause_nvl_gallery_test` use it.
- Threaded tests that wait for a job poll with a timeout (`settle` in `tests/unit/video_test.cpp`).
- Fuzz targets use `vnmaki_add_fuzzer(<name> <sources>)`. ctest gives each target a smoke test of 10,000 runs.
  - A fuzz target checks properties of the output, for example idempotence or an independent oracle. If a property is false, it calls `std::abort()`.
  - `fuzz_image` and `fuzz_webp` share `fuzz/image_properties.hpp` (also run by `tests/unit/image_test.cpp`). `fuzz_webp` adds the RIFF/VP8L container to the input, so the bytes are the chunk data. libwebp is a vcpkg library, so it has no fuzz instrumentation; only the engine code around it has.
  - `fuzz_script_scan` (the Lua tokenizer and the story scan) has `fuzz/script_scan_properties.hpp`, which `tests/unit/script_scan_test.cpp` also runs on samples and on 3000 seeded random scripts. The properties: lines and sizes are in bounds; the tokens written as source (`render_lua_tokens`) tokenize to the same tokens with the same lines and scan to the same labels. Its dictionary is `fuzz/script_scan.dict`.
  - `fuzz_po` (the `.po` reader) has `fuzz/po_properties.hpp`, which `tests/unit/po_test.cpp` also runs on samples and on 3000 seeded random texts. The properties: reading any text either throws `PoError` with a line inside the text, or gives a file that `write_po` writes and `parse_po` reads back to the same entries (and the written text is a fixed point); and a file made from the bytes themselves (any bytes in the texts) does the same round trip. Its dictionary is `fuzz/po.dict`.
  - To check a new fuzz target, add a bug and make sure that the smoke test finds it.
- Threaded tests put a timeout on each wait. If `!JobSystem::threads_supported()`, they return immediately, because jobs run inline.
- GPU tests are in `tests/render/` (`vnmaki_render_tests`).
  - Get a renderer with `vnmaki::test::make_headless_renderer()`.
  - If there is no device, it prints `VNMAKI_SKIP:` and ctest skips the test. `VNMAKI_REQUIRE_GPU=1` stops the skip. Linux CI sets it, because it has lavapipe.
  - Check pixels with `check_golden(read_pixels(target), "<name>")` against `golden/<name>.png`.
  - `tools/generators/render_fixtures.py` makes the golden images with a reference rasterizer.
  - A feature has its own generator, `tools/generators/<area>_fixtures.py`. It imports `Canvas` and the PNG writer from `render_fixtures`. Check with `check_golden_in("<area>", ...)`. Examples: `layered_fixtures.py`, `shader_fixtures.py`, `frame_anim_fixtures.py`, `transition_fixtures.py`, `camera_fixtures.py`.
  - `presentation_fixtures.py` draws the goldens of the presentation demo (1280 by 720, the label `presentation` of `templates/default/`, played by `tests/render/presentation_render_test.cpp` with a fake clock) at three frames: the middle of a face crossfade, the end of a camera pan with the blink on its closed frame, and the sepia layer shader at 0.5. It uses the pictures of `template_assets.py` and the sepia reference of `shader_fixtures.py`, and draws with a fast copy of `Canvas.draw`'s blend. A frame with a shake (a fractional pixel, so a bilinear filter), a transition or the lip sync (audio clock) is not a golden here: `camera_fixtures.py`, `transition_fixtures.py` and `tests/unit/new_project_test.cpp` check those. Clock times in such a test are binary fractions, so the clock adds up exactly.
  - `lipsync_fixtures.py` is stand-alone: it writes `voice.wav` (0.2 s of silence, 0.4 s of a tone, 0.2 s of silence) for `tests/unit/lipsync_test.cpp`.
  - `webp_fixtures.py` is stand-alone. It writes a tiny lossless WebP (VP8L: no transforms, no colour cache, one simple prefix code per channel, so each channel has at most two values) with the standard library only. The test pixels in `image_test.cpp` are the specification.

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
  - `MainLoopHooks::frame_end` gets a `FrameTiming` (`events`, `update`, `draw` as clock differences, and the number of `updates`) at the end of each frame that ran. `draw` is the whole draw hook. The loop reads the clock for the phases only while `frame_end` is set. A frame that quits does not call it.
- Pointer positions are in window pixels. Convert them with `window_to_virtual(compute_viewport(...), pos)`.
- Clicks in the letterbox bars are outside the virtual area.
- Text input is off when no text field has the focus. `UiHost::set_platform(&platform)` does this for the `input` node (see UI).
- `set_fullscreen(bool)` and `set_window_opacity(float)` apply the player's preferences. `HeadlessPlatform` only records them (`fullscreen()`, `window_opacity()`).
- `user_data_directory(organization, application)` (`sdl_platform.hpp`) is SDL's per-user preference path. Saves go there.
- Only one `SdlPlatform` can exist. Other SDL users call `SDL_InitSubSystem` and `SDL_QuitSubSystem`. They do not call `SDL_Quit`.

### Render (`engine/render/`)

- Engine code draws only through `IRenderer` (`renderer.hpp`).
- `SdlGpuRenderer` is the SDL GPU backend. Make it with `create_for_window(platform)` or `create_headless()`.
- A frame:
  1. `begin_frame`.
  2. One or more passes: `begin_pass({.target, .clear})`, then `draw(quads, DrawState)`.
  3. `end_frame()`. It returns `FrameStats` (`passes`, `quads`, `draw_calls`).
- Adjacent quads with the same `DrawState` are one draw call.
- Coordinates are pixels of the pass target. The origin is top-left.
- The GPU uses premultiplied colors. `Image` pixels are premultiplied RGBA8.
- Draw the scene into a render target at virtual resolution.
- Then draw that target to `kScreen` at `compute_viewport(virtual, window_metrics().pixels, mode).rect`.
- Shaders are HLSL in `shaders/`. shadercross compiles them at build time through `vnmaki_add_shaders(...)`.
- Custom fragment shaders include `vnmaki.hlsli`. They can have one constant buffer at `register(b0, space3)`.
- A custom shader can read up to `kMaxShaderTextures` (3) textures. `DrawState::texture` is slot 0 (`sample_sprite`). `DrawState::more_textures` are slots 1 and 2 (`sample_texture1(uv)`, `sample_texture2(uv)`). An id of zero binds `texture` there. Every draw binds all three slots, because `vnmaki.hlsli` declares them all. So a shader reports 3 samplers.
- Game shaders compile at run time, not at build time (`shader_compiler.hpp`).
  - `compile_fragment_shader(name, hlsl)` uses the SDL_shadercross library (`sdl3-shadercross` in `vcpkg.json` is a target dependency, not only a host tool). It returns a `CompiledShader` that owns SPIR-V, MSL and DXIL. `bytecode()` is the view for `IRenderer::create_shader`.
  - `#include "vnmaki.hlsli"` finds the engine header. The build embeds `shaders/*.hlsli` as text (`shader_headers.cpp.in`), so a game needs no engine shader files.
  - A compile error throws `ShaderCompileError`. The message has the file, the line, the column and the compiler message. Example: `shaders/sepia.frag.hlsl:5:18: error: use of undeclared identifier 'x'`.
  - A shader may use up to three textures and one constant buffer at `register(b0, space3)`. Storage textures and buffers are errors.
  - `shader_compiler_available()` is false on macOS, where vcpkg has no `sdl3-shadercross`, and in a build with `VNMAKI_SHADER_COMPILER` off (release). Tests that compile shaders return early then.
  - `Scene::declare_shader` (and `recompile_shader`) loads a shader in this order: `shaders/<name>.frag.hlsl` is compiled when it exists and a compiler is available; otherwise `shaders/<name>.frag.vnshader` is read; otherwise it is an error. So a game with both files compiles the source (a dev build), and a game with only the `.vnshader` file (a release build, a pack) needs no compiler.
  - `precompiled_shader.hpp` is the `.vnshader` file: `write_precompiled_shader(CompiledShader)` and `read_precompiled_shader(bytes)` (throws `ShaderCompileError`). The format: the magic `VNSH`, a u32 version (1), the sampler count and the uniform buffer count (u32), then the SPIR-V, MSL and DXIL as blocks (a u32 length and the bytes). It keeps every format that the compile made (the DXIL block is empty on macOS), so one file is right for every backend.
  - The compiler is only in `engine/render/sdl_shader_compiler.cpp`, because it includes SDL headers.
- `SdlGpuRenderer::texture_memory()` returns `TextureMemory`: the bytes (RGBA8, width x height x 4) and the number of live textures and of render targets, counted when a resource is created and when it is released. A destroy inside a frame counts at `end_frame()`. It is on `SdlGpuRenderer`, not on `IRenderer`.
- `load_image(vfs, jobs, path)` loads an image and returns `JobHandle<Image>`. `decode_image` reads PNG and JPEG (stb_image) and WebP, lossless or lossy, not animated (libwebp). It finds WebP by the `RIFF....WEBP` header, not by the file name. All three are premultiplied after decoding.
- Upload images with `create_texture` on the main thread.
- `SdlGpuRenderer::set_overlay(fn)` sets a hook that `end_frame()` calls once the passes are recorded. It gets the frame's command buffer and the target of the last pass (`GpuOverlayTarget`), so the hook can draw over the screen. It is not called when the screen pass was skipped. `gpu_info()` gives the device and the screen's format. The dev tools use it for ImGui (see Tools). Remove the hook before the renderer is destroyed.

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
- Image name `N` is the VFS file `images/N.png`. If that file does not exist, it is `images/N.jpg`, then `images/N.webp`.
- A tag is unique across all layers.
- `show` defaults: layer `characters`, position `center`, anchor bottom-center.
- `DisplayProperties`:
  - Position is in virtual pixels.
  - Anchor and crop are fractions.
  - Rotation is in degrees, clockwise.
- `Scene` keeps `std::unique_ptr<Displayable>`. `find(tag)` returns a `Displayable*`. Use `dynamic_cast` to get an `ImageDisplayable`.
- The snapshot is a map from a part name to bytes: `displayables` (images), `layered`, `animations`, `tweens`, `shaders` and `camera`. A new feature adds a part. `restore()` ignores a part that it does not know and reads a missing part as empty.
- The snapshot keeps the properties as a replay leaves them. A tween that ends is at its end value. A loop is at the value where it started.
- Tweens write through `Scene::find(tag)->properties()`. `Scene::update(seconds)` moves them. `hide` and `scene` end the tweens of the displayables that they remove.
- `Scene::show` of a tag that is shown does not end its tweens.
- `Scene::restore()` throws `std::invalid_argument` (a failed load: `Saves::load` returns the error and the story stays) when a snapshot names an image, sprite or animation frame or movie that is not in the VFS. It checks before it changes anything.
  - `Scene::set_warning_handler(fn)`: `prepare()` and `update()` then send an image that does not decode (`ImageError`) or a movie that fails (`MovieError`) to `fn`, once for each file. The displayable stays and draws nothing, and the frame goes on. Without a handler they throw. The story window sets it to its `warn`.
- For each frame, call `scene.prepare(renderer, jobs)` on the main thread. Then call `scene.draw(renderer, target)` in a pass on the virtual-resolution target. Give the id of that target: a layer with a shader ends the pass and begins a new pass on `target`.
- Game shaders:
  - `shader(name, { "amount", ... })` declares `shaders/<name>.frag.hlsl` and compiles it when the scripts load. A declaration again with the same uniforms does nothing. The uniforms are floats (at most `kMaxUniforms`, 16) packed in order into `b0` and padded to 16 bytes. `SceneCommands::declare_shader` carries it out and returns the error string.
  - `show(tag, { shader = name, uniforms = { amount = 1 } })` and `layer(name, { shader =, uniforms = })` set them (`SceneCommands::set_shader`). `shader = ""` removes the shader. A `show` without the options keeps them. Uniforms that the call does not name keep their values; a new shader starts with zeros.
  - `DisplayProperties::shader` is the number of the shader (from 1, in declaration order; 0 is the default shader) and `uniforms` are the values. Every displayable draws through `Displayable::draw_state(texture)`, so images and layered sprites get the shader the same way. `Scene::prepare` makes the GPU shaders and gives them to the displayables.
  - A layer with a shader (and displayables) draws into its own render target of the virtual size, and then that target draws with the shader. The layer properties are in `layer_properties(layer)`.
  - The tag `@layer.<name>` is for the uniforms of a layer in `tween`. A `show` tag cannot start with `@layer.`.
  - The snapshot part `shaders` keeps the shader name (not the number) and the 16 uniform values of each displayable and layer. The part is left out when no shader is set. `restore()` ignores a shader that is not declared.
- Transitions (`SceneCommands::transition`, see Anim):
  - `transition(request)` checks the request and the shader or the mask image, and starts a transition. A replay (`TransitionRequest::replaying`) checks it and does not start it. A duration of 0 does not start it.
  - The layers from `background` to `overlay` are mixed. The `ui` and `transient` layers draw over the result.
  - The old frame is `frames_[0]`, a render target of the virtual size. The first change after `draw()` (`scene`, `show`, `hide` or `transition`) draws the scene as it is then into it, in a frame of its own (`capture_old_frame`). It uses the renderer that `prepare()` saw: the renderer must outlive the changes, and the changes must not be between `begin_frame` and `end_frame`. If nothing was drawn yet (a transition at the start of the story), the old frame is empty. `restore()` also means that nothing was drawn.
  - While a transition runs, `draw()` draws the layers into `frames_[1]` (live, so tweens go on). Then it draws one quad over the virtual screen with the transition shader into the pass of `target`, and the `ui` and `transient` layers after it. Texture slot 0 is the old frame, slot 1 the new frame and slot 2 the mask image (the new frame if there is none).
  - Without a transition `draw()` is as before: the layers draw into the pass of the caller, with no extra pass or quad.
  - `update(seconds)` moves the clock. `tweening(kTransitionTag)` is true until the duration has passed. The transition is drawn one more time at progress 1 and ends at the next `update()`. `finish_tweens(kTransitionTag)` and `restore()` end it at once. It is not in the snapshot.
- Camera (`engine/scene/camera.*`, SPEC §10):
  - The camera moves the layers from `background` to `overlay`. `ui` and `transient` stay. `CameraState` has a pose and a parallax factor for each of those five layers. `Scene::camera()` returns it.
  - The pose is a `DisplayProperties`, so a tween reaches it with the tag `@camera` (like `@layer.<name>`). `position` is the pan in virtual pixels, `scale.x` is the zoom (above 0; 1 is none) and `rotation` turns the content clockwise, in degrees. Zoom and rotation are about the centre of the screen. A `show` tag cannot be `@camera`.
  - A positive pan moves the view right and down, so the content moves left and up. The pan of a layer is `parallax * (pan + shake)`. Zoom and rotation do not depend on the layer.
  - `CameraView` maps a point of a layer to the screen. `Scene::draw_layers` draws a layer that the camera moves through a `CameraRenderer`, an `IRenderer` that passes every call on and moves the corners of each quad. So every displayable follows the camera with no code of its own. A layer with a shader draws into its target with the camera applied, and then the target draws as it is. A transition mixes frames that were drawn with the camera.
  - Without a move (a default camera, or a shake of nothing) the layers draw into the renderer of the caller, as before.
  - `SceneCommands::shake(ShakeRequest)` starts a shake: `strength` is a fraction of the virtual height, `duration` is in seconds. The offset is `shake_offset(Shake)`: smooth noise of the time, from a hash of integers (24 points per second), times a factor that falls from 1 to 0. It is a fixed function of the time, never `rand`. The shake clock is `Scene::update`. A new shake replaces the one that runs.
  - A shake is not in the snapshot. A replay checks the request and starts no shake (`ShakeRequest::replaying`).
  - `tweening("@camera")` (and `tweening("")`) is true while a camera tween that ends or a shake runs, so `wait("@camera")` waits for both. `finish_tweens("@camera")` ends them: the tweens go to their end values and the shake stops.
  - `SceneCommands::set_parallax(layer, factor)` sets the factor of a layer (default 1, from -100 to 100). A layer that the camera does not move is an error.
  - The snapshot part `camera` has the pan, the zoom, the rotation and the parallax of the layers that do not have 1. It is left out when the camera is the default, so a game without a camera has the snapshots that it had before. As for other tweens, a tween that ends is at its end value in the snapshot. A loop on `@camera` is in `tweens`. `restore()` also ends a shake.
  - The GPU goldens are from `tools/generators/camera_fixtures.py`. It repeats the shake noise in Python. A change to the noise in `camera.cpp` needs the same change there and in `tests/unit/camera_test.cpp`.
- `SceneCommands` calls return an error string. The prelude raises it at the script line.
- `SceneCommands` calls also run during replay.
- `SceneCommands` has `animation`, `tween`, `finish_tweens` and `tweening` with default no-op bodies, so other implementations do not need an animation system.
- A displayable draws its quads with `draw_state(texture)` (`Displayable`, protected). What a displayable adds to its draws, for example a shader, goes there, so it applies to every kind of displayable.
- `show(tag, { movie = "movies/x.webm", loop = true })` shows a movie (`MovieDisplayable`) in place of an image. `movie` is the Vfs path with its extension. A bad or missing file is an error at the script line (`ShowRequest::movie`; a replay checks only that the file exists). `movie` and `loop` are in the prelude `show_options`.
  - `Scene::prepare` loads a movie on a job (`MovieDisplayable::prepare`) and uploads its pictures. `Scene::update` is its clock. After a restore (a load, a rollback), or when it is shown again, a movie plays from time 0.
  - `Scene::set_movie_audio(audio)` gives the movie its sound (see Video). The `MovieAudio` must outlive the scene, so make `Audio` before `Scene`.
  - A movie that does not loop is what `wait(tag)` waits for, as for a frame animation, and `finish_tweens` ends it. A movie never blocks a wait when it loops.
  - The snapshot part `movies` has the layer, the index in the layer (movies are put back last, so the `layered` index does not count them), the tag, the path, the loop flag and the properties. It has no time.
  - A movie texture is made again for each new picture. A displayable that goes away gives its texture to `Scene::prepare` to destroy.
- Hot reload (dev tools): `Scene::reload_image(path, jobs)` decodes the file again and puts the new texture in every displayable that shows it (`textures_` keeps one texture for each path). The old texture goes to `retired_textures_` and is destroyed by the next `prepare()`. A file that cannot be decoded returns an error and the old texture stays. `Scene::recompile_shader(name)` compiles a declared shader again from its file (`declare_shader_impl(..., force)`); a compile error returns the message and the old shader stays. Both do nothing for an image that was never drawn or a shader that is not declared.
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
- Lip sync: `layered(..., { lip_sync = { group = "mouth", frames = { "closed", "half", "open" }, thresholds = { 0.05, 0.2 }, voice_tag = "eileen" } })`.
  - `group` is a group in `order`. `frames` are two or more value names of it, the closed mouth first. `thresholds` are one fewer than the frames, rising, above 0 and up to 1: the frame is the number of thresholds that the voice level has reached. `voice_tag` is the tag of the voice to follow (the name of the definition if you leave it out). A bad table is an error at the script line (`LayeredRequest::LipSync`, checked in `make_lip_sync` in `scene.cpp`).
  - `Scene::set_voice_level(&audio)` gives the `VoiceLevel`. `Scene::update` gives each lip-synced sprite `voice_level(tag)` (`LayeredSprite::set_voice_level`): the group shows the frame for that level, and the first frame when no voice with the tag plays. The group draws only that frame, with no crossfade, in place of the value that `show` chose.
  - The mouth is not state: `attributes()` and the snapshot keep the chosen value, a new sprite (a `show`, a `restore`) starts with the mouth closed, and a replay plays no voice, so it stays closed. `vnmaki run` also stops the voice channel while skip mode is on.

### Frame animation (`engine/scene/frame_animation.*`)

- Script API: `animation(name, { frames = { "a", "b" }, fps = 12, loop = true })` or `animation(name, { sheet = "x", columns = 4, rows = 2, count = 7, fps = 12, loop = true })`. The images are image names, as for `show`. `fps` is required, from above 0 to 1000. `loop` is true by default. A sheet is read row by row. `count` is the number of cells that are frames (all by default). There are at most 4096 frames. A bad definition or a missing image gives an error at the script line.
- `SceneCommands::animation(AnimationRequest)` makes the definition. `show(tag, { image = name })` (or `show(name)`) shows it, as for an image. A layered sprite takes the place of an animation, and an animation takes the place of an image of the same name. Define an animation before the layered sprite that uses it.
- A `FrameAnimation` is a `Displayable`. It draws one frame through `draw_state`: a whole image, or a cell of a sheet (the uv of the frame, cropped by `crop`). The frame size is the image size, or the cell size. `Scene::prepare` loads all the images of an animation (`missing_paths()` and `set_texture(path, ...)`).
- `Scene::update(seconds)` is its only clock. The frame is `floor(time * fps)`, and it wraps when `loop` is true. If `loop` is false, the last frame stays. `AnimationDefinition::frame_at` has the rule.
- A value of a layered group can be an animation: `eyes = { open = "eyes_open", blink = "blink" }`. It plays from frame 1 when the value is chosen, and a group that keeps its value keeps playing. In a crossfade, the old value shows the frame that it had when it left.
- `wait(tag)` and `wait_all()` wait for an animation that does not loop: `Scene::tweening(tag)` is true until its last frame has played for its full time. A loop never blocks a wait. `finish_tweens` (a click, skip mode) puts the animation at its last frame. The same is true for the animation values of a layered sprite (`LayeredSprite::animating()` and `finish_animations()`).
- The snapshot part `animations` has the layer position, the tag, the definition name and the properties. It has no definition and no time. `restore()` throws if the scene does not know the definition, so the scripts must load first. After a restore (a load, a rollback), an animation restarts at frame 1.

### Video (`engine/video/`)

- A movie is AV1 video and Opus audio in a WebM file. `open_movie(bytes)` (libwebm `mkvparser`) gives a `MovieData`: the encoded video packets with their times, the size, the duration and the audio. `load_movie(vfs, path)` reads it from the Vfs. A bad file throws `MovieError`, and the message is `movie '<path>' ...`. `movie_problem(vfs, path)` gives that text, or an empty string.
  - The duration is the time of the last video packet plus one frame. There must be one AV1 video track. The audio track is optional and must be Opus. Laced blocks are an error.
  - The audio is made again as an Ogg Opus file (`MovieData::audio`), so `AudioDecoder` reads it like any other file.
- `VideoDecoder` (dav1d, one thread, no frame delay) turns packets into opaque RGBA `MovieFrame`s with `decode(n)`, and goes back to the start with `rewind()`. It handles 8 to 12 bit pictures, all chroma layouts, BT.601 and BT.709 (BT.709 by default), and limited and full range.
- `MoviePlayer` plays one movie from time 0. The decoding runs on a job (`JobSystem`): one job at a time makes a few pictures, so the memory stays small. Call `update(seconds)` and `prepare(renderer)` from the main thread.
  - The clock starts when the first picture is decoded. With a `MovieAudio` and a movie that has audio, the audio starts then and the picture follows the audio position (the audio clock). Without audio, or after the audio has ended, the picture follows the seconds that `update` gets (the loop clock). A loop starts again when the clock passes the end, and never finishes.
  - A bad AV1 stream throws `MovieError` from `update`.
- `MovieAudio` (`movie_audio.hpp`) is the interface to the audio. `Audio` implements it: the movie's Ogg Opus plays on the `movie` channel in place of the movie there, and `start_movie_audio` returns a token. The audio is not state: `Audio::restore` stops it, and a `MoviePlayer` stops its audio when it goes away. It is silent, with the loop clock, if the file cannot be played.
- `MoviePresenter::set_warning_handler(fn)`: a movie that fails to load or decode (`MovieError`) goes to `fn`, and the presenter ends that movie and advances the story. Without a handler `update()` throws. The story window sets it to its `warn`.
- `movie(path, { skippable = true })` is a blocking call (`MovieRequest`). `SceneCommands::check_movie` checks the file first, and an error is raised at the script line. It is a step like a `wait`: `advance()` ends it, a replay passes it without playing it, and rollback goes back over it (`ScriptPosition::wait`). `skippable` is false by default.
  - `MoviePresenter` (`movie_presenter.hpp`) plays it in the host. `update(seconds)` once per update, `prepare(renderer)` before drawing, and `draw(renderer, size)` after `Scene::draw` (it draws black and the picture, centered, over everything). `click()` ends a skippable movie and returns true if the story waits at a movie. `skip()` is for skip mode: it ends any movie (`PlaybackOptions::skip_wait`).
  - The headless runner does not play a movie. It writes `movie <path>` to the transcript and goes on.
- Fuzz target: `fuzz_video` (the container, the Ogg audio, the decoder).

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
- Transitions (`transition.hpp`, `shaders/transition_*.frag.hlsl`):
  - Names: `dissolve`, `fade`, `slide_left`, `slide_right`, `slide_up`, `slide_down`, `wipe_left`, `wipe_right`, `wipe_up`, `wipe_down` and `mask`. The direction is the way the content (slide) or the edge (wipe) moves.
  - Options: `duration` (seconds, 0.5 if none), `color` (`fade`: "#rrggbb", black if none), `softness` (`wipe_*` and `mask`: 0 to 1), `mask` (`mask`: an image name; the new image shows first where the mask is dark), `uniforms` (a custom shader). An option that the transition does not use is an error. `make_transition_spec` checks all this without a GPU.
  - Any other name is a game shader (task 24) that includes `transition.hlsli` and declares the uniform `progress`. The engine sets `progress` each frame. `from_at(uv)`, `to_at(uv)` and `mask_at(uv)` read the three textures. The other uniforms come from `uniforms = { name = number }`. A shader that is not declared, or has no `progress`, is an error at the script line.
  - `Transition` is the clock. It has no real clock: the caller gives the seconds to `advance()`. Progress is linear.
  - Script API: `transition(name, options)` and the `with` option of `scene(image, options)`, `show(tag, options)` and `hide(tag, options)`. `with` is a name, or `{ name, option = value, ... }`. `with` starts the transition before the change, so a bad name changes nothing. All the changes of one step (since the last blocking call) use the one transition that the step ends with. A layered sprite cannot have a group named `with`.
  - A transition blocks like `wait`: it is a `WaitRequest` with the tag `kTransitionTag` (`"@transition"`). It is a step also when nothing is shown, so replay counts the same steps. `WaitPresenter::skip()` (a click, skip mode) ends it. `ScriptPosition::wait` marks it, so rollback goes back over it. In a replay the transition is checked and not shown. In skip mode the first tick of a transition can be drawn.
- Camera (`camera.hpp`, see Scene):
  - Script API: `camera { x, y, zoom, rotation, duration, ease, delay }` is `tween("@camera", ...)` that is instant when it has no duration. `x` and `y` are fractions of the virtual width and height, as for `show`. `scale` also works for `zoom`. `shake { strength, duration }` has a strength from 0 to 1 (a fraction of the virtual height, required) and a duration above 0 (0.5 s if none). `layer(name, { parallax = f })` sets the factor of a layer, with or without the `shader` option.
  - `tween("@camera", ...)` allows only `x`, `y`, `zoom` (or `scale`) and `rotation`, and no uniforms. The zoom must be above 0. `wait("@camera")` waits for a move or a shake. `camera` and `shake` do not block. A loop on `@camera` never blocks a wait.
  - `TweenRequest::instant` is `replaying` for a request that is not a replay: the tweens that end go to their end values at once.
- `pause(seconds)` (0 to 86400) is a `WaitRequest` with the tag `kPauseTag` (`"@pause"`) and `WaitRequest::seconds`. `WaitPresenter::update(seconds)` counts the time (give it the frame time; the default 0 never ends a pause with time left) and ends the pause. `skip()` (a click, skip mode through `PlaybackOptions::skip_wait`) ends it at once. A replay does not wait. The headless runner ends it at once and prints `pause <seconds>`.
- `ScriptPosition::wait` marks a wait step (also a pause). Rollback goes back over wait steps to the previous say, narrate, menu or `call_screen` (a wait is never a rollback step).

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
  - `Playback` marks each line it sees in the `PersistentStore`. It takes the script's `skip` and `auto` calls. `vnmaki run`: `A` toggles auto, `S` toggles skip (`toggle_skip`: off, or `kSeen`, or `kAll` when the preference `skip_unseen` is on), and an advance input turns skip off.
  - Preferences: `DialoguePresenter::set_characters_per_second` (text speed) and `Playback::set_auto_base_delay` (auto delay). See Default screens and preferences.
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

- A screen is a `UiNode` tree with a name. Kinds: `box`, `hbox`, `vbox`, `text`, `image`, `button`, `toggle`, `slider`, `bar`, `input`. The Lua constructors have the same names, and `grid { columns = n, ... }` (`build_ui_tree` makes it a `vbox` of `hbox` rows; give the cells one width).
- Lua: `screen "name" { nodes }`, or `screen("name", function(args) return nodes end)`. A table of several nodes is wrapped in a `box`. `show_screen`, `hide_screen` and `call_screen` take the screen name and an `args` table for the function form.
- `build_ui_tree` documents the options (`width`, `padding`, `bg`, `returns`, ...). An unknown option is an error that names the node.
- A text or button string is the first string in the table: `button { "Save", returns = "save" }`. A button hands `returns` to its screen when activated.
- A screen is built when it is shown. Only bound nodes follow data (see the next rules).
- `bind(table, key)` is the option `bind` of a `toggle`, `slider` or `bar`. `table` is `store`, `persistent` or `prefs`. The prelude turns it into the options `bind_table` and `bind_key`.
  - `UiHost::set_bindings(&runtime)` connects them. `UiHost` reads each bound node's datum (`ScriptRuntime::binding`) every time it lays out, draws or handles an event. It writes the datum (`set_binding`) when the player flips the toggle or moves the slider. Without it the node keeps its own value.
  - A toggle follows a boolean. A slider or bar follows a number between its `min` and `max` (default 0 and 1). A `step` snaps a slider. A click or a drag on a slider sets it. Left and Right (also the d-pad) move it by `step` (a twentieth of the range if unset).
  - A write to `store` through `set_binding` while a label waits goes in the input log (`StoreWrite`: key, value and the number of the wait). A rollback or a load makes it again at the same wait, before the answer. Writes to one key at one wait make one entry, so a slider drag stays small. `persistent` and `prefs` are not rolled back and not logged.
- Styles (SPEC §15): `style(name, { font, size, color, background, padding, gap, margin, width, height, grow, align, justify, fill, border = { width, color }, focus = { background, color, border }, hover = { ... }, extends = "other" })` declares a named style. A node takes `style = "name"`.
  - Merge order, lowest first: the built-in look of the node kind (`build_ui_tree`), the styles in the `extends` chain (base first), the style, the node's own options.
  - The prelude does the merge when the node is made. `style` is not an option that reaches C++. A style is flat node options (`background` is `bg`, `border = {...}` is `border_width` and `border_color`, `focus = {...}` is `focus_bg`, `focus_color` and `focus_border`, `hover` is `hover_*`). A style option that does not fit the kind is left out: only a button, toggle, slider or input gets `focus_*`. So one style can serve a text node and a button.
  - An unknown style, an `extends` loop (more than 16 levels), an unknown style option and a style that is not a string are errors at the script line. Styles are looked up when a screen is built, so a style may be declared after the screen that uses it.
  - A focused node uses `focus_*`, a hovered one `hover_*`, else the base. A hovered node that has no `hover_*` uses `focus_*` (pointer hover also moves the focus). `UiStyle` has the fields (`focus_background`, `hover_background`, `focus_color`, `border_width`, `font`, ...). `font` is a family of the `FontSet`.
  - `theme("name")` makes the style table of the engine's default screens, then runs `styles/<name>.lua` (a script that calls `style`). It replaces the game's own styles that were declared before. It is not in a snapshot and `show_screen` is not undone by a rollback, so call it before the story starts, or in the label that changes the look. A screen that is already shown keeps its look until it is built again. A missing or bad file is an error at the script line.
  - The styles that `engine/runtime/default_screens.cpp` declares are the defaults of every theme (the prelude knows them by the chunk name `=engine/screens.lua`). The default screens use only named styles for their look. Position options that depend on the arguments (`x`, `y` and widths of the say box) stay on the node. A game or theme restyles a default screen by declaring a style of the same name again (it replaces the whole style, `extends` can reuse the old one only under another name).
- `input { bind = bind(store, "name"), placeholder = "Name", max_length = 20 }` edits a string datum. The string in the table is the start text of an unbound input. `max_length` counts characters (default 256, at most 4096). `returns` is what Enter gives the screen. An input is focusable, has the role `kTextInput` and `UiNode::label` defaults to the placeholder.
  - It has the keys when it is the focused node of the keyboard screen (`UiHost::wants_text_input()`). A modal screen focuses its first focusable node, so an input there has them at once. On a screen that is not modal, a click on an input gives the screen the keys (like Tab), and Esc takes them back. Moving the pointer over other nodes does not take the focus from an input; a click does.
  - `UiHost` calls `Platform::set_text_input(true)` when an input has the keys and `false` when it loses them (focus moves, a screen is hidden, Esc). It does this after each event, `show` and `hide`. The story window gives it the platform (`ui.set_platform`).
  - While an input has the keys, `TextInputEvent` adds its text at the end (whole characters, no control characters, up to `max_length`), Backspace removes the last character, and the other keys are consumed and do nothing, except Tab, the arrows and Esc (focus and cancel work as before) and Enter. So the story keys (A, S, L, Space) do not fire. The story window does not give events to the backlog while `wants_text_input()` is true. There is no caret movement, selection or paste: the caret is always at the end.
  - A bound input follows the datum (`ScriptRuntime::binding`), a string; any other value reads as empty. A change re-lays the screen out, because an input without a `width` has the size of its text.
- A screen root can have `modal = true` (`show_screen` then shows it modal) and `cancel = value`. Esc or the gamepad's east button give the screen that value, like a button's `returns`.
- Tab or the gamepad's west button give the keyboard to the top non-modal screen that has a focusable node (`UiHost`). It keeps the keys until the player presses Esc or the east button.
- Yoga does the layout (`layout_tree`). Only `ui_layout.cpp` includes it, and it is linked PRIVATE.
  - The root fills the canvas. Lengths are virtual pixels. A node with `x` or `y` is positioned absolutely in its parent.
  - Give `layout_tree` a `TextMeasure`. `UiRenderer::text_measure()` measures with the fonts. `estimate_text_size` is for tests.
- Each node has a `role`, a `label` (default: its text) and a `state` (`focused`, `hovered`, `disabled`). These are the data for the accessibility tree.
- Only enabled buttons take the focus. Tab order is document order. The arrow keys and the d-pad pick the nearest button in that direction. Pointer hover moves the focus.
- `UiHost` is the stack of screens (the last one shown is on top).
  - A screen with the same name is replaced. `handle_event(event, viewport)` returns `consumed` and an optional `ScreenActivation`.
  - A modal screen takes the keys, the d-pad and all clicks. Other screens take the pointer only. While any modal screen is shown, `handle_event` consumes every player input (wheel, Page Up, shoulders, A, S, L too), so nothing reaches the story. Window, gamepad connection and quit events pass.
  - `UiHost` does not store screens in snapshots. `show_screen` and `hide_screen` run again during replay, so they must be idempotent.
- `ScreenPresenter` shows the modal screen of a `menu` or `call_screen` request and answers the runtime. Make it after `runtime.start()`. Call `sync()` after anything else that moves the story. Give it events before the dialogue presenter: if `handle_event` returns true, do not advance. Rollback input (`is_rollback_event`) is the one exception: while its own menu or `call_screen` is the top modal screen and no input has the keys, `handle_event` returns false for it, so the player can roll back from a menu. A modal screen over it (save, load, preferences, confirm) consumes it. `sync()` shows the screen again when the request changes at the same position (a quick-load to the same label and step with other options or another screen tree).
- `UiRenderer` draws trees through `IRenderer`. Call `prepare(root, scale)` before `begin_frame`. Draw into a screen-resolution pass with the viewport scale and origin, like `SayBoxView`.
  - It has one glyph atlas for each text size. A node with `markup = true` uses text tags.
  - An image node draws `images/<name>.png` (else `.jpg`, else `.webp`) from the VFS. It needs `width` and `height`.
- The say box is a screen: `make_say_screen` builds it, and `SayBoxView` lays it out and draws it with a `UiRenderer`.
  - `SayBoxView::set_screen_source` replaces the built-in tree with one from a script. The story window gives it the Lua screen `say`.
  - The tree needs a text node with `reveal = true`: the line. Its size, width and position give the layout, the reveal and `link_at`. If the source gives nothing, the built-in box is used.
- `UiRenderer::set_generated_images` makes the pixels of an image node named `gen:...` (save thumbnails). The texture is kept under the name, so a changed image needs a new name.

### Default screens and preferences

- `engine/runtime/default_screens.cpp` is a Lua chunk. `load_scripts()` runs it before the game's scripts, so a game replaces a screen by defining `screen "name"` again.
  - The screens: `title`, `say`, `choice`, `confirm`, `save`, `load`, `preferences`, `quick_menu`, `nvl`, `gallery` and `gallery_view`. They take their look from named styles that the same chunk declares first (`title_screen`, `say_box`, `panel`, `back_button`, `slot_cell`, `quick_button`, `nvl_screen`, `gallery_cell`, ...; see the top of the file). A new default screen uses named styles only, not inline options.
  - `nvl`: `args.lines` (how many page lines come first), `args.typing` (the last line is the one on the typewriter: its text node has `reveal`), the lines (`speaker`, `text`, `color`) and then the options of a menu (`text`; an option returns its number from 1). `make_nvl_args(page, typing, options)` (`ui_build.hpp`) builds them. The story window builds `nvl` instead of `say` for a line with `DialogueLine::nvl` and draws it through `SayBoxView`. `ScreenPresenter` shows a menu with `MenuRequest::nvl` as `nvl` (modal), so the options are inside the NVL screen. A page has no scroll: scripts call `nvl_clear()`.
  - `gallery` takes no args. It reads `gallery_images()` and `unlocked("cg", name)` itself: a button `@view:<name>` with the image for an unlocked image, a disabled "Locked" button for the others. `@view:<image>` opens `gallery_view` (args `image`; modal, one button `@close`). The title screen shows a Gallery button only when `gallery_images()` is not empty.
  - The backlog is the C++ `BacklogScreen` (`vnmaki:backlog`).
- Screens size themselves by the read-only global `canvas` (`canvas.width`, `canvas.height`). `ScriptRuntimeOptions::canvas_width` and `canvas_height` set it (default 1920 by 1080). Write a screen as a function: `screen("x", function(args) ... end)`.
- `ScriptRuntime::build_screen(name, args, tree)` runs the screen function for the engine. `args` is a `ScreenNodeDef`: its `props` are the named fields of the Lua table, and its `children` are the list (each a table of the same form). The arguments:
  - `say`: `speaker`, `text` (with tags), `color` (`"#rrggbb"`, if the character has one). The `reveal` text node is the line.
  - `choice`: a list of `{ text }`. A button returns the number of its option, from 1. `ScreenPresenter` shows it as `vnmaki:menu`. If the screen fails, it shows `make_menu_screen`.
  - `confirm`: `message`, `yes`, `no`. Esc returns `no`.
  - `save` and `load`: `mode` and a list of `{ slot, title, time, empty, broken, thumbnail }`. The list has the slots 1 to `kScreenSlotCount` (12). `load` also lists `auto` and `quick` if they exist, and disables the slots that are empty or damaged. `thumbnail` is an image name `gen:save:<slot>@<time>`, or `""`.
  - `title`: what `call_screen("title", { title = ... })` gives. Start returns `"start"`.
- A button that returns a string starting with `@` is an engine action. `ScreenActions` (`engine/ui/screen_actions.hpp`) does it:
  - `@open:<screen>` (modal; `save` and `load` get the slot list from `Saves::list()`), `@close`, `@save:<slot>` (a slot in use asks `confirm` first), `@load:<slot>`, `@auto`, `@skip`, `@backlog`, `@quit` (asks `confirm`).
  - `@save!<slot>` and `@quit!` are the confirmed forms.
  - `ScreenPresenter::set_action_handler` gives the activations to it first, so a script's own `call_screen` values must not start with `@`.
- `prefs` is a view of `persistent.prefs` with defaults. A key the player never set reads as its default. `prefs.x = v` writes `persistent.prefs`. A value of the wrong type is a script error.
  - Keys: `text_speed` (characters a second, 40), `auto_delay` (seconds, 1), `volume_music`, `volume_sound`, `volume_voice`, `volume_ambient`, `volume_ui` (0 to 1, 1), `fullscreen` (false), `skip_unseen` (false), `window_opacity` (0.3 to 1, 1), `reduce_motion` (false).
  - The defaults are also in the struct `Preferences`. A test checks that they agree.
- `PreferenceApplier` (`engine/ui/preferences.hpp`) reads the preferences each frame and applies what changed:
  - Text speed to the `DialoguePresenter`, auto delay to `Playback`, volumes to `Audio::set_preference_volume`, fullscreen and window opacity to the `Platform`. A null target is skipped.
  - `reduce_motion` has no target yet. Task 23 reads it through `read_preferences`.
- The story window shows `quick_menu` (top right, not modal) at the start and gives every activation to `ScreenActions`. Auto and skip wait while a modal screen is open.

### Audio (`engine/audio/`)

- `Audio` is the mixer. It implements `SceneState` and `AudioCommands`.
- Set `ScriptRuntimeOptions::audio` to it. Also put it in `scene_state`, inside a `CompositeSceneState` when there is a `Scene`.
- The runtime includes no audio headers.
- Audio name `N` is the VFS file `audio/N.opus`, else `.ogg`, `.flac` or `.wav`. `voice(N)` plays `voice/N`.
- Channels: `music`, `sound`, `voice`, `ambient`, `ui`. A script makes another by naming it. The `movie` channel (`kMovieChannel`) is made when a movie's audio first plays (see Video).
- `AudioOutput::kManual` has no device. `update(seconds)` mixes that much. Tests and headless runs use it.
- `kNullDevice` mixes on a device thread to nowhere. `kDevice` is the system output.
- Call the commands, `update()` and `restore()` from one thread.
- `vnmaki run` in a window makes an `Audio` with the system output (the null device if there is none) and calls `update()` every frame. A headless run uses `kManual` and never mixes.
- Music crossfades. The same music on a channel carries on. The music channel ducks while a voice plays.
- The snapshot has each channel's volume and music. `restore()` stops sounds and voices. `restore()` throws `std::invalid_argument` (a failed load) when a music file of the state is not in the VFS, before it changes anything. `Audio::set_warning_handler(fn)`: `update()` sends a music file that does not decode to `fn` (it throws `AudioError` without a handler). The story window sets it to its `warn`.
- Music tracks change at the next `update()`. A replay then gives the same music, so it does not stop.
- The runtime does not pass `sound` and `voice` on during replay.
- `set_preference_volume(channel, level)` is the player's volume for a channel. It multiplies the script's `volume` and the ducking. It is not in the snapshot, so a load or rollback leaves it.
- `voice_level(tag)` (`VoiceLevel`, `voice_level.hpp`) is the RMS level (0 to 1) of the voice that has `tag`, or empty when none plays. A voice gets its tag from the runtime: a `say` with a `voice_tag` after a `voice(name)` call (and not in a replay) calls `AudioCommands::tag_voice(tag)`. The mixer sums the squares of the decoded samples, before volume, in windows of 960 frames (20 ms), as it consumes them. So the level follows the clock of `update()` (manual output) or of the device thread (device and null device), and it has no delay beyond the window. A voice that ends has no level.
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
- `ScriptObserver::on_label_entered` is called for `start()`, for each `jump` and `call` target, and for `replay()` to step 0. `on_label_resumed` is called when a called label returns. Neither is called while `replaying()`. `on_replay()` is called first in every replay (a load, a rollback), before any label of it. `TraceRecorder` (see Story graph) reads them.
- `label(name, fn, { ending = true })`: the third argument has no effect at run time (the prelude ignores it). The story graph reads it from the source as the mark of a label that ends the story on purpose. The template marks `warm`, `cold` and `presentation` with it, so `vnmaki check` gives no finding.
- The sandbox has no `io`, `debug`, `package` or `require`. `vfs.*` is the only file access.
- `store` is the only game state.
  - It keeps booleans, numbers, strings and tables with scalar keys.
  - A different value stops the story at the next label entry.
  - `rand` is `math.random`. Its state is `store.__rng`.
- `eval(code)` runs Lua for the dev REPL (see Tools) and `set_observer(observer)` changes the observer after the runtime was made.
  - `eval` runs in the sandbox of the scripts, with the instruction and memory limits. It tries `return <code>` first, then `code` as statements. It returns `EvalResult` (`ok`, and `text`: the values separated by tabs, or the error). It never throws. A table is listed one level deep.
  - A blocking call (`say`, `menu`, ...) in `eval` is an error, because no label is running.
  - A change to `store` by `eval` stays until the next rollback or load, because it is not in the input log. `persistent` and `prefs` keep it.
- `ScriptRuntime::replaying()` is the only replay flag.
  - Subsystems update their state during replay.
  - They check the flag only for effects that the player can see or hear.
- `replay(LabelSnapshot, inputs, step)` restores the label-entry snapshot and replays the top label only.
  - It does not make the callers again.
  - Only menu choices, `call_screen` results and bound `store` writes (`StoreWrite`) go into the input log. A replay makes the writes of a wait when it reaches that wait, then gives the answer. A save writes both: `[0, index]` and `[1, value]` (nil, boolean, integer, float or string, as MessagePack types). `[2, [wait, key, value]]` is a store write. A reader rejects any other tag.
- Scene state goes into snapshots through `SceneState` (`capture()` and `restore(bytes)`) in `ScriptRuntimeOptions::scene_state`.
- `menu { "a", "b" }` returns a 1-based number. C++ gives the answer with `choose(index)`, which is 0-based. `ScreenPresenter` (`engine/ui/`) shows the menu and calls `choose`.
- `call_screen(name, args)` is a blocking call (`CallScreenRequest`). C++ gives the value with `return_screen(value)`. The value is nil, a boolean, a number or a string.
- `ScriptRuntimeOptions::screens` (`ScreenCommands`, implemented by `UiHost`) carries out `show_screen` and `hide_screen`.
  - `check_screen` runs on every tree first. Its error string is raised at the script line.
  - The runtime includes no ui headers. It reads the Lua node tables into `ScreenNodeDef`.
- Reduced motion (SPEC §17): `engine.reduce_motion()` in the prelude is true when `persistent.prefs.reduce_motion` is set (not nil, not false). It is false when `persistent.prefs` is not a table or has no such key, so camera code works before and after the preferences (task 18) put `prefs` in `persistent`. Camera calls read it when they run, so a change takes effect at the next call.
  - A tween of `@camera` (also `camera`, and each `@camera` item of `sequence` and `parallel`) gets a duration of 0, no delay, no loop and no yoyo. `tween` and `camera` also make it instant. The `wait` step stays, so a replay counts the same steps.
  - `shake` checks its arguments and starts nothing (`ShakeRequest::replaying`).
  - Other motion (tweens of displayables, transitions) is not changed by this flag yet.
- `label_names()` lists the labels, sorted. `label_source_hash(name)` covers the lines of the label function. It does not cover the helpers that it calls.
- `wait(tag)`, `wait_all()` and transitions (`transition(...)`, the `with` option) are blocking calls too (`WaitRequest`; a transition has the tag `@transition`). `advance()` ends them. See Anim.
- `movie(path, { skippable = })` is a blocking call (`MovieRequest`) that works as a wait for rollback and replay. See Video.
- `say` with a character that has a `voice_tag`, right after a `voice(name)` call, tags the playing voice (`AudioCommands::tag_voice`). It does nothing in a replay. See Audio and Layered sprites.
- The headless runner prints a transcript (`label X`, `say Who: text`, ...). `vnmaki run <dir> --headless` runs it. It ends a wait at once and needs no input for it.
- NVL mode (SPEC §7). The page is `store.__nvl = { on = bool, { speaker =, text =, color = }, ... }`, written by the prelude, so snapshots, saves, rollback and load cover it with no change to the save format. `nvl(true)` sets `on`; `nvl(false)` removes the page; `nvl_clear()` empties it. A line is NVL when `on` is set or the character has `kind = "nvl"` (`"adv"` is the default). The prelude adds the line to the page, then `engine.stage_nvl(page)` gives it to the next `say`, `narrate` or `menu`, which put it into `SayRequest::nvl`, `NarrateRequest::nvl` or `MenuRequest::nvl` (a menu only in NVL mode). The request holds a copy of the page, with the current line last. `DialoguePresenter` copies it into `DialogueLine::nvl`, so a line with the same text on another page is another line for `SayBoxView`. The backlog needs nothing: NVL lines are say and narrate steps.
- Unlocks: `unlock(kind, name)` sets `persistent.unlocks["<kind>/<name>"] = true`; `unlocked(kind, name)` reads it. `ScriptRuntimeOptions::gallery` (`project.toml` `gallery = ["cg/a", ...]`: image names, as for `show`) is the list; `scene` and `show` unlock `cg/<image>` for an image in it. A replay writes it again (idempotent). `gallery_images()` returns the list.
- `project.toml` has `title` (required), `version`, `resolution`, `languages`, `gallery`, `auto_save` (a list of `"label"` and `"menu"`), `rollback_depth` and `backlog_length` (each 0 to 10000, default 100).
- `save(slot)`, `load(slot)`, `quick_save()` and `quick_load()` only queue a `SaveRequest`. `take_save_requests()` gives them to the host. They are dropped during a replay.
  - A slot is an integer from 1. `load` with a number is a slot. With a string or function it is Lua's `load`.
- `on_load(function(old_version, store) ... end)` registers the migration hook while the scripts load. `run_on_load(snapshot, old_version)` runs it on the store of a snapshot, not on the live `store`.
- `style`, `theme`, `input` and the style merge of the node constructors are in the prelude, in a literal of their own before the screens literal. `engine.load_theme(name)` reads `styles/<name>.lua` through the VFS and returns the compiled chunk (or the reason).
- `bind`, `prefs`, `canvas`, `toggle`, `slider`, `bar` and `grid` are in the prelude (the part after `local persistent` is its own raw literal; MSVC limits one literal to 16380 bytes). `ScriptRuntime::binding(table, key)` and `set_binding(table, key, value)` read and write a bound datum for the UI.
- `persistent` is a second Lua table, like `store`, but it is not in any snapshot or save. Rollback and load never change it.
  - `persistent_snapshot()` and `restore_persistent()` give the host the data. Call `restore_persistent` before `load_scripts()`.
  - A replay runs script code again, so code that writes to `persistent` must be idempotent (`persistent.won = true`, not `persistent.n = persistent.n + 1`).
- Rollback keeps its history in the runtime. `roll_back()` and `rollback_steps()` are the interface.
  - A step (`RollbackStep`) is a position where the story waited at a say, narrate, menu or `call_screen`. A `wait` is not a step. It holds the label-entry snapshot and input log of the outermost running label, the number (`ordinal`) of the yield point, and `voice`, the last `voice(name)` before it (see Backlog in Dialogue). So each step stands alone, and the history works across `jump`, `call` and `ret`.
  - `roll_back()` replays the outermost label from its snapshot, with the labels it called, to that yield point (`begin_replay` with an ordinal). Replay does not report the label entries, and it does not add a step. The observer then hears the request, as for a load.
  - From a say or menu it goes to the step before. From a wait it goes to the last step. It returns false when there is nothing to go back to, and then it changes nothing.
  - The history keeps `ScriptRuntimeOptions::rollback_depth` steps back (default 100, `project.toml` key `rollback_depth`, 0 is off: `can_roll_back()` is false everywhere, also at a wait). The oldest steps go first.
  - A rollback drops the later steps at once. There is no roll forward. The player who goes back to a menu and chooses again gets a new branch with no trace of the old one.
  - `menu { "a", "b", fixed = true }`: after the player answers it, the history is empty. Rollback cannot go back to the menu or before it. `fixed` must be a boolean.
  - `replay()` (a load) empties the history. The story then starts a new one.
  - Rollback and `persistent`: rollback never changes it. A replay runs script code again, so the idempotence rule for `persistent` is also true here.
  - `show_screen` and `hide_screen` are not in snapshots (see UI), so a rollback does not undo them.
  - The headless runner has `RollbackInput`. It fails the run when there is nothing to go back to.
- `skip("seen" | "all" | "off" | false)` and `auto(bool)` queue a `PlaybackRequest`. `take_playback_requests()` gives them to the host. They are dropped during a replay.
- `character { auto_multiplier = n }` goes to `SayRequest::auto_multiplier` (default 1).
- `drop_callers()` makes the running label the outermost label.
- `begin_load()` is `drop_callers()` that keeps the callers and the rollback history aside. A load calls it before `replay()` and `end_load(undo)` after. If the replay fails and the story goes back to its old position, `end_load(true)` puts the callers and the history back, so a failed load changes nothing. Otherwise they are dropped.
- `reload_scripts()` (dev tools, hot reload) loads all scripts again and replays to where the story waits.
  - The new scripts run in a fresh `Impl` (a new Lua VM). `persistent` is copied into it before `load_scripts()`. A script error (also a failing `shader(...)` call) returns the error (`script:line: message`). The old `Impl`, the story and the scene stay. The runtime swaps `impl_` only when the replay worked, so a pointer from `request()` or `label_snapshot()` is not valid after it.
  - The replay is the one of `roll_back()`: the outermost label (with its input log and `stops` count) to the same yield point, so a `call` stack comes back. The observer hears the request it stops at (a text change shows at once). A replay does not report label entries.
  - Label gone: the story goes to `start`, with the store of a new game and an empty scene. If `start` is also gone, the reload fails and the scene is put back.
  - A replay that cannot reach the yield point (the label is shorter, a menu is new, the code fails) is tried again to the last yield point that it reached (`clamp_replay_`, set in `fail()`). If that fails too, it goes to `start`.
  - The rollback history stays, except the entry for the position that the replay pushes again, so the number of steps does not grow. Queued save and playback requests stay. After a failed state the history is empty.
  - Do not call it from a script, an observer or during a replay.

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
- This module has no UI. The `save` and `load` screens show the slot list (`Saves::list()`); see Default screens and preferences.
- `PersistentStore` (`persistent_store.hpp`) keeps `persistent` and the seen steps in `<directory>/persistent.vnpersist`, in the same directory as the saves.
  - The file is plain MessagePack (`encode_persistent`, `decode_persistent` in `save_file.hpp`). A bad file (damaged, or of another `format`) is reported through `on_warning`, renamed to `persistent.vnpersist.bad`, and the game starts with no data. The first `flush()` does not write the defaults; only a later change writes a new file.
  - `load()` runs before `load_scripts()`. `flush()` writes the file only if `persistent` or the seen set changed. Call it whenever the story waits. The destructor calls it too.
  - A seen step is `{label, step, source hash of the label}` (`SeenStep`). An edited label has a new hash, so all its steps are unseen.
- Hosts call `Saves::poll()` and `PersistentStore::flush()` once for each frame (`vnmaki run`) or after each input (`run_headless` with `HeadlessOptions::saves`).

### `vnmaki new` and templates (`engine/runtime/new_project.*`, `cli/new_command.*`, `templates/`)

- `vnmaki new <name>` copies `templates/default/` to `<name>`. The title is the last part of `<name>` (`project_title`). It stops (exit 1, nothing written) if the target exists and is not an empty folder.
- `create_project(template_dir, target_dir, title)` replaces `{{title}}` in `.toml` and `.lua` files. It escapes `\` and `"` for a quoted string, which is the same in TOML and Lua. Other files are copied byte for byte.
- `find_template(name, executable_dir, source_templates_dir)` tries `<folder of vnmaki>/templates/<name>`, then the source tree. The CLI target gets the source tree path from the definition `VNMAKI_SOURCE_TEMPLATES_DIR`. `executable_directory()` is in `sdl_platform.hpp`.
- A template is a game: `project.toml`, `scripts/`, assets. It uses the default screens of the engine and does not define its own. The `default` template has two script files: `main.lua` (the sample story) and `presentation.lua` (the label `presentation`, the Phase 3 acceptance demo, reached from the third option of the menu in `classroom`). Add a template as a folder of `templates/` with a `project.toml`.
- The binary assets of a template come from a generator, not from files that someone drew or downloaded. `tools/generators/template_assets.py OUT_DIR` writes the assets of `templates/default/` (it only adds files). Run it on `templates/default` again after you change it, and commit the result. `.gitattributes` keeps `templates/` free of line-ending changes.
  - It reuses `text_fixtures.build_font` for `fonts/default.ttf`, a 5 by 7 pixel font (ASCII only). A glyph shape can be a list of boxes.
  - It also writes the parts of the layered sprite (eyelids and mouth frames for the blink animation and the lip sync), the backdrop `bg/rooftop`, the mask `mask/iris`, the voice line `audio/voice/eileen/hello.wav` (tone bursts of known levels) and `shaders/sepia.frag.hlsl` (the text of `shader_fixtures.SEPIA_SHADER`). `movies/clip.webm` is the only template file that is not generated.
  - A test (`unit.template: the assets are ...`) runs the generator into `fixtures/template` and compares it with the template. PNG files are compared by name only, because zlib versions differ.
  - An asset must be MIT, OFL or CC0, or be made by a generator. `templates/default/ASSETS.txt` records the license.
- The end-to-end test is `tests/unit/new_project_test.cpp` (`SampleGame`): `create_project`, then a `ScriptRuntime` with `Scene`, `Audio` (manual output, as the voice level for lip sync) and `Saves` (in a temporary directory) plays both branches, rolls back over the menu, saves, loads and rolls back again. It plays the presentation demo too: it saves while a tween runs (a load leaves the tween at its end value), rolls back over the dissolve and over the mask transition, and steps a fake clock (`SampleGame::step`) to see the mouth frames. `SampleGame::settle` ends waits and passes movies. It needs no window and no GPU, so it runs on every CI platform. `cli.new_makes_a_game_and_stops_at_a_second_one`, `cli.new_game_runs_in_a_window` and `cli.new_presentation_runs_in_a_window` (GPU tests) use `tests/cli/run_new.cmake`; with `-DPRESENTATION=ON` it changes the title screen call into `auto(true) jump("presentation")` so the window plays the demo.
- `docs/script-reference.md` lists the presentation calls (tween, transitions, layered sprites, animations, camera, shaders, movies, lip sync). A change to one of those calls updates it.
- The sample story starts with `call_screen("title", ...)`, so `vnmaki run <game> --headless` stops there. Use a unit test to play it. The menu in `classroom` has three options; the third (index 2) jumps to `presentation`.

### Story graph (`engine/tools/script_scan.*`, `story_graph.*`)

- These two pairs of files need no ImGui and build in every preset (a release build has `vnmaki graph`). `VNMAKI_DEV_TOOLS` guards only the overlay files of the next section.
- `tokenize_lua(source)` (`script_scan.hpp`) never throws and accepts any bytes. Tokens: name, number, string (decoded, except `\u{...}`), one-byte symbol. A line ends at LF, CR, CRLF or LFCR. A string or a long bracket that is not closed ends at the end of the line or of the source.
- `scan_labels(source)` finds `label`, `jump`, `call`, `ret`, `menu`, `say`, `show`, `narrate`, `scene`, `music`, `sound`, `voice` and `movie` calls in the function of each `label("name", function() ... end)`. It runs nothing.
  - A call is `name(...)`, `name "text"` or `name { ... }`, and not after `.` or `:`. `ScanCall::arguments` has one entry for each top-level argument: its text if it is one string literal, else nothing. `ScanCall::option_keys` has the `name =` keys at the top level of a table argument. `ScanCall::depth` counts `function`, `if`, `do` and `repeat` blocks open inside the label function.
  - `ScanCall::speaker` (`say` only) is the first argument as a string literal's text or as a name (`eileen`, `chars.eileen`); it is empty for `nil` or any other value. `ScanCall::table` (`menu` only) has the positional values of the table argument (`ScanElement`: the text of a string literal, else nothing, and the line); it is empty when the argument is not a table written in the call.
  - `scan_screens(source)` finds `screen "name" { ... }` and `screen("name", function ... end)` and, in each, the first positional value of every `text { ... }` and `button { ... }` (`ScanScreen`). A `text` after `.` or `:` is not a node.
  - A label with a computed name has no name. A `label` call inside a label function is not a label; its calls belong to the outer label. `ending = true` in a table (the third argument) after the function marks `ScanLabel::ending`.
- `build_story_graph(read_scripts(vfs))` (`story_graph.hpp`): `read_scripts` lists `scripts/` as the runtime does (all `.lua` files, sorted by path).
  - Edges: `jump`, `call`, and `menu` (a `jump` or `call` that is deeper than the last `menu` call of the same label, so it is in a block that opens after it). A `menu` edge does not say if it is a jump or a call. A target that is not a string literal is a `dynamic` edge (`to` is empty).
  - A label is unreachable if no path of edges from `start` reaches it (dynamic edges lead nowhere). It is a dead end if it has no `jump` and no `ret`, is not `ending`, and no `call` names it. A label that a `call` names runs to its end and returns, so it is not a dead end. This does not find a branch that runs to the end while another branch jumps.
  - The first definition of a label wins. A label with a computed name is not in the graph.
- Run-time traces: set `TraceRecorder` as the `ScriptRuntimeOptions::observer`, or call it from the observer. It records the labels entered and the edge from the label that ran to the one entered, once each. A `ret` goes back to the caller, so the next edge starts there. The runtime does not report the labels of a replay. `ScriptObserver::on_replay()` calls `break_trace()`, so the next label entered after a load or a rollback has no edge into it (a load adds no false edge; the edge out of the label where a replay stopped is lost).
  - `apply_trace(graph, trace)` marks the labels and the edges seen. A traced edge that has no edge in the scripts is added as `kTrace`. It also makes the labels that traced edges reach reachable. It starts from the scripts each time, so call it again with a longer trace.
  - The overlay window "Story graph" (`DevOverlay::set_story(vfs, trace)`) lists the labels (red: unreachable, yellow: dead end) and the edges, and shows what the run took (`seen`) in green. It applies the trace again when `TraceRecorder::revision()` changes; "Rescan" reads the scripts again.
  - `ObserverChain` (`observer_chain.hpp`) gives every `ScriptObserver` call to each observer in a list. `ScriptRuntimeOptions` has one observer slot, so the story window sets a chain of the `LogBuffer` and the `TraceRecorder`.
- `to_dot(graph)` and `to_csv(graph)` print the graph. CSV: `type,from,to,script,line,flags` (RFC 4180, LF), a row for each label and then for each edge. Flags join with `;`. The text of both is in `story_graph.hpp`.
- `vnmaki graph <dir> [--format dot|csv]` (`cli/graph_command.*`) prints the graph of the scripts of the project folder (DOT by default). It runs no story, so it shows no traces. Exit 0, 1 if `<dir>` is not a folder, 2 for a usage error.
- The tests: `tests/unit/script_scan_test.cpp` and `tests/unit/story_graph_test.cpp` (they also read `templates/default`: no unreachable label), and the CLI tests `cli.graph_*` on `tools/generators/graph_fixtures.py`. The generator writes the Lua and notes the line of each edge while it writes. A small model of the rules gives the expected CSV and DOT, so the tokenizer is not its own oracle. The `random` project comes from a fixed seed.

### `vnmaki check` (`engine/tools/check.*`, `cli/check_command.*`)

- Builds in every preset, like the story graph. `check_project(vfs)` returns `Finding`s (`file`, `line`, `severity`, `code`, `message`) sorted by file and line. It runs no story. `findings_to_csv` prints the header `file,line,severity,code,message` and a row for each finding (RFC 4180, LF; the line is empty when 0).
- `vnmaki check <dir>` prints the CSV. Exit 0 when there is no error (warnings are fine), 1 when there is an error or `<dir>` is not a folder, 2 for a usage error.
- It reuses: `read_scripts`, `scan_labels` and `build_story_graph` (edges, reachability, dead ends), `parse_markup`, `load_project_config`, and the lookup rules of the engine, which are public for this: `resolve_image` (`scene.hpp`), `resolve_audio` and `kVoicePrefix` (`audio.hpp`). A new asset rule goes into those functions, not into `check.cpp`.
- Codes. Errors: `lua-syntax` (a `sol::state` loads each script and runs nothing; the line comes from the Lua message), `unknown-label` (a `jump`, `call` or menu branch to a literal label that no script defines), `bad-text-tag` (the literal text of `say` and `narrate`: the `MarkupError` and its byte offset), `missing-image` (a literal name in `scene` or `show`), `missing-audio` (`music`, `sound`, `voice`; a voice is under `audio/voice/`), `missing-movie` (`movie`: the path as written), `po-syntax` (a `translations/*.po` file that `parse_po` rejects: its `PoError` and line), `bad-translation-tag` (a translation in `translations/*.po` whose text tags do not parse: the key, the `MarkupError` and its byte offset; the line is the `msgid`). Only a translation in use is checked: not fuzzy, not obsolete, not empty, and not a `screen/` key (a screen string has no tags).
- Codes. Warnings: `unreachable-label`, `dead-end`, `dynamic-edge` (a `jump` or `call` with a computed target), `project-config` (the `ProjectConfigError` of `project.toml`, with its line when it has one; a missing file or key has none), `non-literal-string` (the text of `say` or `narrate`, an option of a `menu` table, a `menu` argument that is not a table written in the call, or the first value of a screen `text` or `button`, that is not one string literal: `vnmaki extract-strings` cannot extract it).
- Only string literals are checked. `show("x")` is not checked when `x` is the first argument of a `layered(...)` or `animation(...)` call in any script, or when its table has the key `image` or `movie` (the scan does not read the values of a table). A script with a syntax error is still scanned, so its other findings show.
- The template of `vnmaki new` must give no finding: its three endings are marked `{ ending = true }`. Tests: `tests/unit/check_test.cpp` (the template and some cases) and the CLI tests `cli.check_*` on `tools/generators/check_fixtures.py`: one project and one expected CSV for each code, and `mixed` (CSV quoting, order). The generator writes the lines itself by searching its scripts for a marker, so the scan is not its own oracle.

### Localization (`engine/l10n/`, `cli/extract_command.*`)

- Builds in every preset (it only needs the scan in `engine/tools/`). The source language is the first of `languages` in `project.toml`; it has no `.po` file.
- `.po` files (`po.hpp`): `parse_po(text)` throws `PoError` (line from 1) and `write_po(file)` writes LF. A `PoFile` is a list of `PoEntry`. The header is the entry with an empty `context` and `id`, so its comments stay. An entry has translator comments (`# `), tool notes (`#. `), references (`#: `, one word each), flags (`#, `), `context`, `id`, `str` and `obsolete` (`#~`). The reader sets `PoEntry::line` (the line of the `msgid`); it is not compared.
  - No plural forms. `#|` and `#~|` lines and a UTF-8 byte order mark are skipped. A string takes the C escapes (`\n \t \r \a \b \f \v \\ \" \ooo \xHH`). The writer escapes every control byte, so any bytes round-trip. A text with a line break inside is written as one string for each line.
  - The reader is strict, so that a rewrite loses nothing: a comment with no message after it, a `msgid` with no `msgstr`, a string with no keyword and an unknown escape are errors.
- Keys (`extract.hpp`): `translation_key(scope, speaker, text)` is `<scope>/<hash>`: 8 hex digits of FNV-1a (32 bit) over the speaker, a NUL byte and the text. The scope is the label name or `screen/<name>`. The speaker is the first argument of `say` as the scan reads it, so a renamed character variable changes the keys (they then move as fuzzy, see below).
  - Only non-empty string literals are extracted: `say(who, text)`, `narrate(text)`, the options of `menu { ... }` and the first value of a screen `text { ... }` or `button { ... }`. A text with the same key twice is one entry with a reference for each line.
- `extract_template(scripts)` gives the template: a header, then the entries in the order of the scripts (by path) and of the lines. `#.` has `speaker: <name>` and `#:` has `file:line`.
- `update_translation(template, old, language)` gives the new `.po` file.
  - An entry keeps the translation, translator comments and flags of the old entry with the same key and text. The references and notes come from the template.
  - An old entry with a translation whose key is gone moves to a new key that has no translation, in the same scope and at the same position among the entries of the scope. The new entry is `fuzzy`.
  - Any other old entry with a translation becomes obsolete and stays at the end of the file. It comes back (not fuzzy) when its key is extracted again. A gone entry with no translation is dropped.
  - The header of the old file stays. A second run on the same scripts changes nothing.
- `vnmaki extract-strings <dir>` writes `translations/template.pot` and `translations/<lang>.po` for each language after the first. It prints one line for each file (`path: N strings`, and `, F fuzzy, O obsolete` for a `.po` file).
  - It works out all files before it writes one, so a `.po` file that cannot be read (exit 1, `vnmaki: translations/ja.po:5: ...`) changes none. It writes `<file>.tmp` and renames it. A language code that is not letters, digits, `-` and `_` is an error. Exit 0, 1 for an error, 2 for a usage error.
- Tests: `tests/unit/po_test.cpp`, `tests/unit/extract_test.cpp` (the hash vectors come from Python) and the CLI tests `cli.extract_strings_*` on `tools/generators/l10n_fixtures.py`. The generator writes a project with an old `ja.po`, and the expected files from its own model of the hash and the file layout. `tests/cli/run_extract.cmake` runs the tool on a copy, compares the files, runs it again, and then runs `vnmaki check`.

### Asset import (`engine/assets/`, `cli/import_command.*`)

- Builds in every preset. Dev mode loads the loose files of the project. Import makes the files that a release build and a pack use, and keeps them in a cache (SPEC section 19).
- `Importer(vfs, project_dir)` (`importer.hpp`) works on a project that `vfs` serves. `import_all()` imports every file that has an importer. `import_changed(paths)` imports a batch of the file watcher. Both return an `ImportReport`: a list of `ImportedFile` (`source`, `path`, `data`, `cached`) and the `problems` as `<path>: <message>`. One bad file does not stop the others. A bad `project.toml` throws (`ProjectConfigError`, `ImportSettingsError`).
  - The importers (the second column is the virtual path of the output):

    | Source | Output | Importer and version |
    | --- | --- | --- |
    | `X.wav`, `X.flac` | `X.opus` | `audio` 1 |
    | `X.ttf`, `X.otf` | `X.<language>.ttf` or `.otf`, for each language of `project.toml` | `font-subset` 1 |
    | `shaders/X.frag.hlsl` | `shaders/X.frag.vnshader` | `shader` 1 |
    | `X.gltf`, `X.glb` | none: the file is only checked | none |

  - Any other file has no importer, and a pack takes it from the project as it is. For the first three kinds, a pack takes the output in place of the source.
  - Raise the version of an importer when its output changes. The cache then misses.
- The cache (`import_cache.hpp`) is `<project>/.vnmaki/cache/`: one file for each entry, named `<importer>-<16 hex digits>`. The digits are an FNV-1a (64 bit) hash of the source bytes, the importer name, its version and the settings that change the output (`params`), each followed by a NUL byte. The `params` are: for audio, the mode and the target of `[build.audio]`; for a font, the language and the list of its characters; for a shader, the text of the engine headers (`vnmaki.hlsli`, `sprite_fragment.hlsli`, `transition.hlsli`). The hash is not secure. `store` writes `<key>.tmp` and renames it. The importer does not list the folder `.vnmaki`, and a watcher batch ignores it. Add `.vnmaki/` to the `.gitignore` of a project.
- Audio (`audio_import.hpp`): `import_audio(bytes, settings)` decodes with `AudioDecoder` (so it also takes Vorbis and Opus) and encodes Ogg Opus with libopusenc at its default bitrate. The output is always stereo at 48 kHz (a mono file becomes two equal channels). The gain comes from `project.toml` (`import_settings.hpp`, which reads the file by itself, because `ProjectConfig` ignores unknown keys):

  ```toml
  [build.audio]
  normalize = "peak"   # "none" (the default), "peak" or "rms"
  target_db = -1.0     # dBFS, -120 to 0; the default is -1 for "peak" and -18 for "rms"
  ```

  - `peak`: the largest absolute sample reaches `target_db`. `rms`: the RMS level of all samples reaches it, but the gain is at most `1 / peak`, so no sample clips. A silent file has gain 1. Opus is lossy, so the level of the decoded file is right to a few percent only.
  - Loop tags are not copied to the Opus file.
- Fonts (`font_import.hpp`): `subset_font(bytes, codepoints)` keeps the glyphs of the characters with HarfBuzz subset (the layout tables and the box of `.notdef` stay). `language_codepoints` gives the characters of a language: ASCII (0x20 to 0x7E), the title, every string literal of the scripts (`\u{...}` decoded; the source text is what a missing translation shows) and, for a language after the first, the `msgstr` of `translations/<language>.po`. Every `.ttf` and `.otf` file of the project gets a subset for each language. A changed script, `.po` file or `project.toml` imports the fonts again (the cache makes that cheap). The text module does not choose `X.<language>.ttf` yet: the files are for the pack (task 45) and a later change of the font loader.
- glTF (`gltf_import.hpp`): `validate_gltf(vfs, path)` returns the problems of a `.gltf` or `.glb` file (cgltf). These are errors: a missing external buffer or image (the uri is relative to the folder of the file; `data:` uris are in the file), a buffer shorter than the file says, bad JSON, glTF 1.0, and what `cgltf_validate` finds. cgltf reads the buffers through the Vfs.
- Shaders: `shaders/X.frag.hlsl` is compiled with `compile_fragment_shader` and written as a `.vnshader` file (see Render). Without a shader compiler it is an error ("this build has no shader compiler"), unless the cache has the entry.
- `vnmaki import <dir> [--out <folder>]` prints one line for each file: `<source> -> <output> (imported)` or `(cached)`, and `<source> (valid)` for a glTF file. Problems go to stderr as `vnmaki: <problem>`. Exit 0 when every file imported, 1 when `<dir>` is not a project or a file failed, 2 for a usage error. `--out` writes the outputs to `<folder>` at their virtual paths: a folder to mount over the project, or to pack.
  - Choice: import is a command of its own, not a part of `build`. `vnmaki build <platform>` (task 45) calls `Importer` and packs the outputs.
  - To try the load of a precompiled shader in a release build: run `vnmaki import <project> --out <out>` with a debug build, make a copy of the project without `shaders/*.hlsl`, put the `.vnshader` files from `<out>` in it, and run it with the release `vnmaki run`.
- Dev mode (`engine/tools/hot_import.*`, dev tools only): `HotImport(vfs, project_dir, log)::apply(batch)` calls `Importer::import_changed` for a batch of the file watcher (task 32). The log gets an info entry `imported <source> -> <output>` for each new import (a cache hit says nothing) and an error entry `import: <problem>` for a failure. `story_window.cpp` calls it after `HotReload::apply`. It leaves `.hlsl` files to HotReload, which compiles them and reports their errors. It runs in the frame that polls the watcher, so a long audio file stalls one frame.
- Tests: `tests/unit/import_test.cpp` (the cache key, hit and miss, the gain of each mode and of a generated WAV, the glyph count of a subset, the characters of a language, glTF errors, the `.vnshader` file and its load by `Scene`, the project importer after a changed file), `tests/unit/hot_import_test.cpp` (dev builds), and the CLI tests `cli.import_*` (`tests/cli/run_import.cmake`: two runs on a copy, the outputs, the cache entries, the second run from the cache). `tools/generators/import_fixtures.py` writes the WAV files (`struct`), the glTF files (`json` and `struct`) and the projects. The font and the shader come from `templates/default/`, because Python cannot write them. The tests of `.vnshader` output need a shader compiler and return early without one.

### Tools (`engine/tools/`)

- Dev builds only, except the Lua scan, the story graph and the profiler (no ImGui; see Profiler). `engine/CMakeLists.txt` adds the module when `VNMAKI_DEV_TOOLS` is ON. A release build compiles none of the rest, links no ImGui, and `cli/story_window.cpp` guards the overlay calls with `#if VNMAKI_DEV_TOOLS`.
- `ImguiBackend` (`imgui_backend.*`) owns the ImGui context and the SDL GPU backend. Only `imgui_backend.cpp` includes SDL (`lint.no_sdl_outside_platform` allows it). Its header has no ImGui and no SDL.
  - It draws through `SdlGpuRenderer::set_overlay`, so the ImGui frame goes over the last pass of the renderer frame (the screen pass in the game). Per frame: `begin_frame(pixels, seconds)`, the windows, `end_frame()`, then the renderer's `end_frame()` draws it.
  - ImGui does not read SDL events. `DevOverlay::handle_event` gives it the engine's `InputEvent`s. Text arrives as `TextInputEvent`, so the overlay turns `Platform::set_text_input` on while ImGui wants text.
  - There is no `imgui.ini`. Destroy it before the renderer. Only one can exist.
- `DevOverlay` (`overlay.*`) is the set of windows (Console, and Story graph after `set_story`). Panels are drawn in `draw()`. Add a panel as a `draw_<name>()` function and call it there.
  - F12 shows and hides it (`handle_event` returns true for F12, so the game never gets it). A hidden overlay does no ImGui work.
  - While it is visible, `handle_event` returns true for the pointer events over an overlay window and for the keys and text that a text field of the overlay wants. The game then gets no input. Clicks outside the windows go to the game. ImGui's wants flags are from the last frame.
  - In `story_window.cpp`: `DevOverlay` is made after the renderer, `handle_event` runs first in `on_event`, and `draw(window_pixels)` runs before `renderer->end_frame()`.
- `LogBuffer` (`log_buffer.*`) is the log: the last 1000 entries with a level (`kInfo`, `kWarning`, `kError`). It is a `ScriptObserver`: Lua `print` is info and dev-mode warnings are warnings. `story_window.cpp` also adds the warnings of the saves and the persistent store. The console has a filter for each level and a Clear button.
- The REPL is the input line of the console. `run_repl_line` logs `> line`, then the values (info) or the error (error), by `ScriptRuntime::eval`.
  - It runs in the script sandbox: the same globals, limits and no `io`, `debug`, `package` or `require`. `print` goes to the log.
  - A change to `store` stays until the next rollback or load, which restore the snapshot of a label entry. `persistent` and `prefs` keep their changes. A new global gives the dev-mode warning.
  - A blocking call is an error.
- Inspectors (`inspect.hpp`, `inspect_*.cpp`): each panel is a function that returns rows and uses no ImGui. The overlay draws the rows in one window, `Inspectors`, with a tab for each. `DevOverlay::set_inspect_targets(scene, audio)` gives it the `Scene` and `Audio`. A null target makes its tabs say so.
  - `inspect_store` and `inspect_persistent` (`StoreRow`: `depth`, `key`, `value`, `is_table`) list a snapshot depth first. A table that two fields share is expanded once, and the other field says `(same table as #n)`.
  - `inspect_scene` lists all layers, back to front, with the tag and kind of each displayable. `inspect_properties(scene, tag)` lists its properties by name (`x`, `alpha`, `tint_r`, `crop_x`, ... and `uniform_n` when it has a game shader). `set_property` writes one at once and keeps `alpha` and `tint_*` in [0, 1].
  - A property edit is live. It is not in the snapshot of the label, so a rollback or a load brings back the old value (the panel says this). A tween that writes the property also overwrites it.
  - `inspect_tweens` lists the running tweens from `Timeline::active()` (`Scene::timeline()`). `inspect_audio` lists a row for each playing track (file, channel volume, position in seconds) and for each silent channel. `inspect_labels` lists `ScriptRuntime::label_names()` and marks the label that the story waits in.
  - `jump_to_label(runtime, name)` is the "Jump" button. It calls `drop_callers()`, then `replay()` of a snapshot of `name` with the current `store` and the scene bytes of the running label's entry, no inputs, to step 1. The story is then at the first blocking call of the label. If the label has none, it returns the error and replays the story back to where it was (the callers are then gone, as after a load).
- Hot reload (`file_watcher.*`, `hot_reload.*`). A change to a project file reaches the running game.
  - `FileWatcher(root)` watches the project folder (recursively) with efsw. The efsw thread only calls `notify(path)` (path from the root, `/` separators). `poll()` runs on the main thread and returns the paths as one batch (sorted, each once) when no event came for `kWatchBatchWindow` (100 ms). `FileWatcher()` has no operating system watch, so a test injects events with `notify(path, time)` and `poll(time)`.
  - `HotReload::apply(batch)` runs between frames (the `update` hook in `story_window.cpp`):
    - `shaders/<name>.frag.hlsl` calls `Scene::recompile_shader`; an image (`.png`, `.jpg`, `.jpeg`, `.webp`) calls `Scene::reload_image`; any `.lua` file calls `ScriptRuntime::reload_scripts()` once, last (see Runtime). Other files do nothing.
    - The default screens and styles reload with the scripts, because `reload_scripts()` loads them as `load_scripts()` does. The host's own screen (`quick_menu`) is opened again through `set_on_scripts_reloaded`.
    - `persistent` does not change and no rollback step is added (see Runtime).
  - An error is logged as an error entry (with the `script:line` that Lua gives, or the shader file and line) and shows a red banner: the screen `vnmaki:hot_reload` of the `UiHost`, so the banner also shows when the overlay is hidden. It names the first error and counts the others. A file that works again clears its error.
  - `run_story_window(vfs, project_dir, ...)` gets the folder to watch. If efsw cannot watch it, hot reload is off and a warning goes to the log.
  - A game shader that a script declares again with other uniforms compiles while the scripts load. If a later script then fails, the scene keeps the new shader (the shader table is not part of the rollback of a failed reload).
- Dev mode asset import (`hot_import.*`): see Asset import.
- Tests: `tests/unit/repl_test.cpp` (REPL and log), `tests/unit/inspect_test.cpp` (the row functions, on the template in `templates/default`), `tests/unit/hot_reload_test.cpp` (injected batches and edits of a temporary project, with a counting renderer for the texture and shader checks), `tests/platform/file_watcher_test.cpp` (real writes, Windows) and `tests/render/overlay_render_test.cpp` (a headless GPU frame, F12, input, a click on each inspector tab). All are only built with the tools.
- Profiler (`profiler.*`, `engine/core/load_timing.*`). `Profiler(clock, {.enabled, .max_frames})` records a `FrameSample` for each frame (`events`, `update`, `prepare`, `draw`, `updates`, the `FrameStats` and the `TextureMemory` of the frame) and the asset loads. It keeps the last `max_frames` frames (0 keeps all) and at most 10000 loads. A disabled profiler records nothing and sets no load sink.
  - It has no ImGui. It builds in every preset. Only the overlay panel is dev-only. `vnmaki run <dir> --frames N --profile out.csv` therefore works in a release build too, which is the build whose numbers matter. `--profile` needs a window (not `--headless`).
  - `story_window.cpp` makes the profiler first (so that it sees the font loads): in a dev build it is always enabled, for the overlay (last 300 frames). In a release build it is enabled only by `--profile`, and then it keeps all frames. The wiring: `hooks.frame_end` calls `end_frame(timing)`; the draw hook calls `begin_prepare()` and `end_prepare()` around the uploads before the first pass (`prepare` is that time, and `draw` is the rest of the draw hook), and `set_gpu(end_frame(), texture_memory())` after the renderer's `end_frame()`.
  - CSV (SPEC §5): `out.csv` has `frame,events_us,update_us,prepare_us,draw_us,total_us,updates,passes,quads,draw_calls,texture_bytes,target_bytes,textures,targets` and a row for each frame. `out_loads.csv` has `path,time_us` and a row for each load, in the order they ended. The files are written when the window closes. Times are whole microseconds.
  - Asset loads: a `LoadTimer` (`engine/core/load_timing.hpp`) in `load_image`, `load_font`, `load_movie` and the audio track loader times the read and the decode. It reports to one global sink (`set_load_sink`), because loads run on job threads. With no sink it costs one atomic read. Only one enabled profiler can exist. The loader of a new asset kind adds a `LoadTimer`.
  - The panel "Profiler" (`DevOverlay::set_profiler`, `draw_profiler`) is drawn last in `draw()`: a frame-time graph of the last 300 frames, the phase times of the last frame, the GPU counters and the memory, and the 8 slowest loads.
- Profiler tests: `tests/unit/profiler_test.cpp` (a fake clock with exact phase times, the CSV text, the load timing; in every build), `tests/platform/main_loop_test.cpp` (`frame_end` timing), `tests/render/renderer_test.cpp` (the texture byte count) and `cli.run_profile_writes_a_row_for_each_frame` (the CSV header and the row count).
