# Build

This guide has two parts: how to build the engine from the source, and how to build a game for players with `vnmaki build windows`. The engine builds and ships for Windows first. The build rules for contributors are in `docs/CONVENTIONS.md` (Build and test, Asset import, Pak and `vnmaki build windows`).

## Build the engine

The engine is C++20 with SDL3 and Lua 5.4. You need Visual Studio (the MSVC x64 compiler, CMake and Ninja that come with it) and vcpkg. The Visual Studio vcpkg works. Run every command through `tools\dev\vsenv.cmd`, which sets up the MSVC environment (`cl`, `ninja`, `VCPKG_ROOT`):

```
tools\dev\vsenv.cmd cmake --workflow --preset debug
```

A workflow configures, builds and runs the tests. To build only:

```
tools\dev\vsenv.cmd cmake --preset debug
tools\dev\vsenv.cmd cmake --build --preset debug
```

The result is `build\<preset>\cli\vnmaki.exe`. A preset has its own folder, so `debug` and `release` do not disturb each other.

| Preset | Use |
|--------|-----|
| `debug` | Development. Warnings are errors. It has the dev tools (overlay, debugger, hot reload, file watcher) and the run-time shader compiler. |
| `release` | The build that players get: optimized, no dev tools and no shader compiler. |
| `asan-ubsan`, `tsan`, `fuzz`, `clang-tidy` | Sanitizers, fuzz targets and clang-tidy. CI runs them. On Windows they do not run locally. |

Each preset has a `system-*` version that finds installed libraries with `find_package` and does not use vcpkg.

Before a push, run `tools\dev\check.cmd`. It runs the `debug` and `release` workflows, as the Windows jobs of CI do. While you work, run only the tests of the code that you changed:

```
ctest --test-dir build/debug -R <pattern> --output-on-failure
```

### Options

| Option | Default | Meaning |
|--------|---------|---------|
| `VNMAKI_DEV_TOOLS` | on in a debug build, off in a release build | Builds `engine/tools/` with Dear ImGui, efsw and nlohmann-json: the overlay (F12), the console, the debugger, hot reload and the launcher window. |
| `VNMAKI_SHADER_COMPILER` | on in a debug build, off in a release build | Links SDL_shadercross, which compiles game shaders (`shaders/*.frag.hlsl`) at run time and in `vnmaki import`. |
| `VNMAKI_PLUGINS` | none | The C++ plugins of `plugins/` that are linked into the runner (`docs/plugins.md`). Use a build folder of its own: `cmake --preset debug -B build/debug-plugins "-DVNMAKI_PLUGINS=example"`. |

## Make, check and play a game

```
vnmaki new my_game
vnmaki check my_game
vnmaki run my_game
```

`vnmaki new` copies the template (`docs/getting-started.md`). `vnmaki check` lints the project and prints a CSV of findings: it exits with 0 when there is no error. If the game has more than one language, `vnmaki extract-strings my_game` makes the translation files (`docs/localization.md`).

`vnmaki launcher` (or `vnmaki` with no arguments in a debug build) opens a window with a button for each of these commands and for Build windows. Each button calls the same function as the command line.

## Build the game for players

```
vnmaki build windows my_game --out my_game_build
```

The command does three things:

1. **Imports** every asset of the project with the importer (below). A problem stops the build with exit code 1, and no pack is written.
2. **Packs** the project into `my_game_build\game.vnpak`. The pack has every file of the project, with these changes: the output of an importer takes the place of its source, and the top-level folders that start with `.` (`.vnmaki`, `.git`) and the output folder itself (when it is inside the project) are left out. Scripts, plugins, images, models, movies, `translations/` and `project.toml` go in as they are.
3. **Copies the runner** as `<title>.exe` (the characters that Windows does not allow in a file name become `_`), with every `.dll` of the folder of the runner.

It prints `packed N files into <path>` and the path of the runner. Exit codes: 0, 1 (no project, an import problem, no runner, a failed write) and 2 (usage). `windows` is the only platform.

### The runner

The runner is the **release** `vnmaki.exe`, because players get no shader compiler and no dev tools. The build itself runs from a debug build. By default, the runner is the `release` folder next to the folder of the running build: `build\release\cli\vnmaki.exe` when you run `build\debug\cli\vnmaki.exe`. If the file is not there, the command stops and prints the path. Build the `release` preset too, or pass another runner:

```
vnmaki build windows my_game --out my_game_build --runner build\release\cli\vnmaki.exe
```

A game that has game shaders needs a build with the shader compiler (a debug build) to import them. A release `vnmaki` cannot import a shader: it plays the `.vnshader` files that a debug build made. A project with no shader file builds with any `vnmaki`.

### What the player gets

Give the output folder to the player.

- `<title>.exe` started with no arguments, or with only options (`--frames`, `--headless`, `--data-dir`), plays the `game.vnpak` in its folder. It does not open the launcher.
- `vnmaki run <folder>` does the same for a folder that has a `game.vnpak`: the packs are the whole project, and the loose files of the folder are ignored. A pack that cannot be read gives exit code 1 and a message.
- Saves and the `persistent` data are in the same place as in development: the per-user data folder for the organization `VNMaki` and the title of the game. `--data-dir <path>` changes it.
- `game.vnpak` is a file of zstd frames with an index. Each file has a hash, so a damaged file is an error value and not a crash. There is no encryption: the format is not a way to hide assets.

### Patches

A file `patch-*.vnpak` next to `game.vnpak` overrides the files of the game by path. The packs are mounted in this order: `game.vnpak`, then the patches in byte order of their names (`patch-10` comes before `patch-2`, so give the names the same width). A later pack wins. Other `.vnpak` files are not used.

## The importer

`vnmaki import <dir> [--out <folder>]` runs the importer alone. It prints one line for each file (`(imported)` or `(cached)`). `--out` also writes the outputs to `<folder>` at their paths in the pack. `vnmaki build windows` calls the same importer.

| Source | Output in the pack |
|--------|--------------------|
| `X.wav`, `X.flac` | `X.opus` (Ogg Opus, stereo, 48 kHz) |
| `X.ttf`, `X.otf` | `X.<language>.ttf` or `.otf`, a subset for each language of `project.toml` |
| `shaders/X.frag.hlsl` | `shaders/X.frag.vnshader` (compiled) |
| `X.gltf`, `X.glb` | the file itself; the import only checks it |

Any other file goes in as it is. A bad file is a problem that stops the build, and the other files are still imported. The results are cached in `<project>/.vnmaki/cache/`: a file is imported again only when its bytes or the settings that change the output change. Add `.vnmaki/` to the `.gitignore` of the project.

`project.toml` can set the loudness of the audio that is imported:

```toml
[build.audio]
normalize = "peak"   # "none" (the default), "peak" or "rms"
target_db = -1.0     # dBFS, -120 to 0; the default is -1 for "peak" and -18 for "rms"
```

The font subsets keep the ASCII characters and the characters of the title, of the string literals of the scripts and of the translations of each language (`docs/localization.md`). A game that runs from loose files uses the whole font.

A font with a Reserved Font Name (SIL OFL) must not be changed and keep that name, and a subset is a changed copy. List such a font to have it copied unchanged, with no subset:

```toml
[build.fonts]
keep = ["fonts/dyslexia.otf"]   # OpenDyslexic, Reserved Font Name "OpenDyslexic"
```

`vnmaki import` prints `<font> (kept)` for it. The importer cannot find a Reserved Font Name by itself: the license text says it, and the font file often does not. The template lists OpenDyslexic. Noto Sans JP (the font of the Japanese add-on, `vnmaki new <name> --lang ja`) has the Reserved Font Name "Source", which its name does not use, so the build subsets it.

## Try the whole path

The end-to-end test of the content demo does these steps on a copy of the template with the Japanese add-on: `vnmaki new --lang ja`, `vnmaki build windows`, it deletes the project, and it plays the label `content` from `game.vnpak` in English and in Japanese (`tests/unit/content_demo_test.cpp`). To do the same by hand:

```
vnmaki new my_game --lang ja
vnmaki build windows my_game --out my_game_build
my_game_build\my_game.exe
```
