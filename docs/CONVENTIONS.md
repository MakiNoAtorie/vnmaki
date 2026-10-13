# Engine conventions

The working rules for contributors and agents. The spec is `docs/SPEC.md`. Keep this file short: add a line when a ticket sets a rule that later work must follow, and delete lines that stop being true.

## Build and test

- Presets (Ninja, output in `build/<preset>/`): `debug`, `release`, `asan-ubsan`, `tsan`, `fuzz`, `clang-tidy`. Running `cmake --workflow --preset <name>` configures, builds and runs ctest in one step.
- Plain preset names get dependencies from vcpkg (`VCPKG_ROOT`).
- Each preset has a `system-*` twin that finds dependencies installed on the system instead, with `find_package(<pkg> CONFIG REQUIRED)` (CMake's FindLua for Lua). Every dependency must be findable both ways.
- `fuzz` and `clang-tidy` are Linux and LLVM Clang only. Pick Clang with `CC=clang CXX=clang++`.
- On Windows, `tools\dev\vsenv.cmd cmake --workflow --preset debug` builds with MSVC and the vcpkg that Visual Studio bundles. Sanitizers, clang-tidy and fuzzers then run only in CI. In cloud containers, `tools/dev/bootstrap-deps.sh` builds the dependencies for the `system-*` presets.
- Before pushing, run `debug`, `asan-ubsan` and `clang-tidy` (plus `tsan` for threaded code and `fuzz` for loaders), then `git ls-files -z '*.cpp' '*.hpp' | xargs -0 clang-format --dry-run --Werror`.
- CI (`.github/workflows/ci.yml`) runs:
  - Windows/MSVC: debug, release.
  - Linux/Clang 18: asan-ubsan, tsan, fuzz, release, and a no-threads build.
  - macOS/Apple Clang: debug, asan-ubsan.
  - A clang-tidy job and a format check.
- CI clones vcpkg at the `builtin-baseline` in `vcpkg.json`. A new vcpkg dependency that needs system packages on a runner needs an install step in the workflow, e.g. `.github/actions/install-sdl3-deps`.
- vcpkg pins: Lua is overridden to 5.4.8, because the spec requires 5.4. `sdl3` is built without default features, with `x11`/`wayland` on Linux. mpack has no vcpkg port, so it gets vendored into `third_party/mpack/` (MIT).

## Code

- C++20, namespace `vnmaki`. Types are `PascalCase`, functions and variables `snake_case`, enumerators `kName`. Scoped enums get a small base type such as `: std::uint8_t`. Files are `.hpp`/`.cpp`, formatted Google style at 100 columns.
- Includes use the path from the repo root (`"engine/vfs/vfs.hpp"`). Order: the file's own header, then `<std>`, then `<third/party.h>`, then `"engine/..."`.
- Every warning is an error, and clang-tidy runs with `WarningsAsErrors: '*'`. Fix findings rather than suppress them.
  - Name literal constants, even in tests. Literals inside doctest `CHECK(...)` are fine.
  - Call `reserve()` before a `push_back` loop.
  - Give struct fields default initializers (`PointF position{};`).
- The engine is a single static library, `vnmaki_engine`. Each module has `engine/<module>/CMakeLists.txt`, which adds its files with `target_sources(vnmaki_engine PRIVATE ...)` and is listed with `add_subdirectory` in `engine/CMakeLists.txt`.
  - Third-party libraries are linked PRIVATE, and engine headers never include them.
  - First-party targets link `vnmaki::options`, which carries the warnings, hardening and sanitizer flags. Never link it into third-party code.
- Game-facing code reads files only through `Vfs`. Nothing outside `engine/platform/sdl_*` (and the files in the renderer's allow-list) includes SDL; the ctest `lint.no_sdl_outside_platform` enforces this.
- The version is set once, by `project(vnmaki VERSION ...)`, and read through `vnmaki::engine_version()`.

## Tests

- Tests use doctest.
  - Unit tests live in `tests/unit/*_test.cpp` and run in the `vnmaki_unit_tests` executable. Each test shows up in ctest as `unit.<name>`.
  - Platform tests live in `tests/platform/` and run in their own executable. On Linux they use `SDL_VIDEO_DRIVER=offscreen`.
  - Every doctest executable needs `DOCTEST_CONFIG_USE_STD_HEADERS`.
- CLI tests use `vnmaki_add_cli_test(name ARGS ... EXIT n [STDOUT_LINE t | NO_STDOUT] [STDERR_REGEX r])`, or `vnmaki_add_cli_stdout_file_test(... STDOUT_FILE f)` to compare against a file.
- Test fixtures are never hand-made. A Python 3 script (standard library only) under `tools/generators/` takes `OUT_DIR` as its only argument and writes them. It is registered with `vnmaki_add_fixture_generator(<name> SCRIPT ... TEST_TARGETS ...)`, and tests read from `VNMAKI_FIXTURES_DIR "/<name>"`.
- Script tests use `vnmaki::test::memory_vfs({{path, text}...})` from `tests/support/memory_vfs.hpp`.
- Fuzz targets use `vnmaki_add_fuzzer(<name> <sources>)`, and ctest gives each one a 10,000-run smoke test.
  - A fuzz target should check properties of the output (idempotence, an independent oracle) and call `std::abort()` when one is violated.
  - To check a new fuzz target works, plant a bug and confirm the smoke test catches it.
- Threaded tests put a timeout on every wait. They return early when `!JobSystem::threads_supported()`, because jobs run inline in that build.
- GPU tests live in `tests/render/` (`vnmaki_render_tests`). Get a renderer with `vnmaki::test::make_headless_renderer()`, which prints `VNMAKI_SKIP:` (ctest reports a skip) when no device exists, unless `VNMAKI_REQUIRE_GPU=1` (set on Linux CI, which has lavapipe). Check pixels with `check_golden(read_pixels(target), "<name>")` against `golden/<name>.png` from `tools/generators/render_fixtures.py`, whose reference rasterizer repeats the scene.

## Module contracts

- **VFS** (`engine/vfs/`): a stack of `VfsMount`s, where later mounts override earlier ones by path. It offers `read`, `exists` (files only) and `list` (merged, sorted).
  - Paths are canonical UTF-8, separated by `/`, and case-sensitive everywhere. Anything that could escape the mount or fail on Windows is rejected (see `path.hpp`).
  - Const methods are safe to call from several threads at once; `mount()` is not.
  - A `.vnpak` archive will plug in as another `VfsMount`.
- **Jobs** (`engine/jobs/`): `JobSystem` returns a `JobHandle<R>` with `ready`, `wait`, and `get` (can be called once; rethrows the job's exception).
  - Subsystems take a `JobSystem&` rather than owning one.
  - A job must never wait on another job's handle, because that can deadlock.
  - Use `JobHandle` rather than `std::future` for results that cross threads (`std::future` caused a TSan race).
  - `VNMAKI_ENABLE_THREADS=OFF` builds run every job inline.
- **Platform** (`engine/platform/`): `Platform` (SDL or headless) turns OS events into a `std::variant` called `InputEvent`. `MainLoop` runs a fixed timestep: events, then updates, then `draw(alpha)`, with an injectable clock.
  - Pointer positions are in window pixels. Map them with `window_to_virtual(compute_viewport(...), pos)`; clicks in the letterbox bars fall outside the virtual area.
  - Text input is off unless a text field has focus.
  - Only one `SdlPlatform` can exist. Other SDL users call `SDL_InitSubSystem`/`SDL_QuitSubSystem`, never `SDL_Quit`.
- **Render** (`engine/render/`): engine code draws only through `IRenderer` (`renderer.hpp`). `SdlGpuRenderer` is the SDL GPU backend (`create_for_window(platform)` or `create_headless()`).
  - A frame is `begin_frame`, then passes (`begin_pass({.target, .clear})` followed by `draw(quads, DrawState)`), then `end_frame()`, which returns `FrameStats` (`draw_calls`). Consecutive quads with an equal `DrawState` are one draw call.
  - Coordinates are the pass target's pixels, origin top-left. Colours are premultiplied on the GPU; `Image` pixels are premultiplied RGBA8.
  - Draw the scene into a virtual-resolution render target, then draw that to `kScreen` at `compute_viewport(virtual, window_metrics().pixels, mode).rect`.
  - Shaders are HLSL under `shaders/`, compiled by shadercross at build time with `vnmaki_add_shaders(...)`. Custom fragment shaders include `vnmaki.hlsli` and may take one constant buffer at `register(b0, space3)`.
  - Images load with `load_image(vfs, jobs, path)` (`JobHandle<Image>`); upload with `create_texture` on the main thread.
- **Runtime** (`engine/runtime/`): `ScriptRuntime` loads every `.lua` file under `scripts/` through the VFS. Each label runs as a coroutine.
  - The script-facing API is a Lua prelude in `script_runtime.cpp`, backed by private `engine.*` primitives.
  - Every blocking call goes through `Impl::yield_at`. That function counts the step (steps are per label activation and start at 1) and calls `ScriptObserver::on_blocked`.
  - `jump`, `call` and `ret` are transfers, not steps.
  - The sandbox has no `io`, `debug`, `package` or `require`, and `vfs.*` is the only file access.
  - `math.random` is a stub until the state ticket (search for `kRandomStubSeed`).
  - The headless runner prints a transcript (`label X`, `say Who: text`, ...), and `vnmaki run <dir> --headless` drives it.
  - `project.toml` holds `title` (required), `version`, `resolution` and `languages`.
