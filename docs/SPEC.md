# VNMaki — Engine Specification

**Status:** Draft 1, full product specification. We will divide it into versions later.
**License:** MIT
**Purpose:** A public, open-source visual novel engine. The author also uses it to build their own VN.

## 1. Goals

- A portable visual novel (VN) engine with a native core. No heavy embedded interpreter.
- Plain Lua scripts for writers. Full programmatic power when needed.
- Fix these Ren'Py gaps: debug tools, story overview, UI tooling, web and console portability, localization workflow, accessibility, asset pipeline, 3D and rigged-puppet support.
- Every bundled dependency is MIT-compatible. Users supply proprietary SDKs, such as Steamworks and console SDKs, as optional modules.

## 2. Non-goals

- Ren'Py compatibility or `.rpy` import.
- Physics, IK, pathfinding, tile maps, or a real-time gameplay framework. Minigames can use Lua and plugins.
- 3D modeling or animation authoring. External tools make 3D content. The engine plays it back.
- Live2D support.

## 3. Platforms

| Phase | Targets |
|---|---|
| First | Windows, Linux, macOS |
| Later | Web (Emscripten), Android, iOS |
| Final | Nintendo Switch, PlayStation, Xbox (via SDL console ports and platform SDKs, under NDA, out of the public repo) |

Design rules for the later targets:

- The runtime needs no dynamic code loading. Consoles forbid it.
- The engine has no JIT.
- Game code uses the VFS for all file access. It does not call `fopen`.
- The renderer and platform services use thin interfaces.

## 4. Tech Stack

| Area | Choice | License |
|---|---|---|
| Language | C++20 (C++23 where all target compilers support it) | — |
| Build | CMake (presets) + Ninja + ccache | — |
| Dependencies | vcpkg, manifest mode | — |
| Window, input, gamepad, platform | SDL3 | zlib |
| GPU | SDL3 GPU API behind a VNMaki renderer interface | zlib |
| Shaders | HLSL, compiled with SDL_shadercross to SPIR-V / MSL / DXIL | zlib |
| Scripting | Lua 5.4 (PUC) | MIT |
| Lua binding | sol2 | MIT |
| UI layout | Yoga (flexbox) | MIT |
| Text shaping | HarfBuzz | MIT |
| Glyph rasterizing | FreeType | FTL (BSD-style) |
| Line breaking | libunibreak | zlib |
| Bidirectional text | SheenBidi | Apache-2.0 |
| Audio | miniaudio | MIT-0 |
| Audio codecs | Opus, Vorbis | BSD |
| Video | AV1 via dav1d, WebM container via libwebm | BSD |
| Images | stb_image (PNG/JPEG), libwebp | MIT / BSD |
| GPU textures | KTX2 + Basis Universal | Apache-2.0 |
| 3D models | glTF 2.0 via cgltf | MIT |
| Math | glm | MIT |
| Rigged 2D puppets | Inochi2D (via its C API, or a native C++ runtime if integration proves impractical) | BSD-2 |
| Save serialization | MessagePack via mpack | MIT |
| Compression | zstd | BSD |
| Config | TOML via toml++ | MIT |
| Dev tool UI | Dear ImGui | MIT |
| Accessibility tree | AccessKit (C bindings) | MIT/Apache |
| File watching | efsw | MIT |

## 5. C++ Guardrails

- Warnings are errors: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (Clang/GCC), `/W4 /WX /permissive-` (MSVC).
- Build on at least Clang and MSVC.
- Debug builds use the hardened standard library (`_GLIBCXX_ASSERTIONS` / libc++ hardening mode).
- clang-tidy checks: `bugprone-*`, `cppcoreguidelines-*`, `modernize-*`, `performance-*`, `concurrency-*`. Second pass: cppcheck or MSVC `/analyze`.
- Enforce clang-format.
- ASan and UBSan in debug and CI builds. TSan for threaded subsystems.
- Fuzz targets for every loader of untrusted data: saves, scripts, packs, images, glTF, Inochi2D puppets, translation files.
- Code rules: RAII. No raw `new` or `delete`. `std::unique_ptr` by default. `std::span` or `std::string_view`, not a pointer and a length. Lua C API only through sol2 wrappers.
- CI (GitHub Actions): Windows/MSVC, Linux/Clang with sanitizers, macOS/Clang, clang-tidy, format check.
- Generator scripts make test fixtures. Do not make fixtures by hand. Benchmark and tool outputs use CSV.

## 6. Architecture

```
┌───────────────────────────────────────────────┐
│ Game scripts (Lua)          Dev tools (ImGui) │
├───────────────────────────────────────────────┤
│ Script runtime · State/Saves/Rollback · Story │
├───────────────────────────────────────────────┤
│ Scene graph · Displayables · Timeline/Anim    │
│ UI (screens) · Text · Audio · Video · 3D      │
├───────────────────────────────────────────────┤
│ Renderer iface · Platform iface · VFS · Jobs  │
├───────────────────────────────────────────────┤
│ SDL3 / SDL GPU / OS                           │
└───────────────────────────────────────────────┘
```

Core modules:

- **runtime** — Lua VM, script loader, label registry, coroutine scheduler.
- **state** — store, persistent store, snapshots, saves, rollback, replay.
- **scene** — layers, displayables, transforms, camera.
- **anim** — timeline, tweens, transitions.
- **ui** — screen tree, layout, styling, focus navigation, input routing.
- **text** — shaping, layout, glyph atlases, text effects.
- **audio** — channels, mixing, voice, streaming.
- **video** — AV1/WebM decoding to textures.
- **model3d** — glTF loader, skinning, morph targets, 3D scenes.
- **puppet** — Inochi2D displayable.
- **render** — renderer interface + SDL GPU backend.
- **platform** — window, input, platform services (achievements, cloud saves).
- **vfs** — loose files in dev, `.vnpak` archives in release.
- **jobs** — thread pool for asset decoders and loaders.
- **tools** — debugger, story graph, hot reload, inspectors (dev builds only).

**Displayable interface:** Visible items (images, layered sprites, text, video, Inochi2D puppets, 3D scenes, UI nodes, plugin content) implement one interface: update, draw, bounds, tweenable properties.

## 7. Scripting

### Model

- Plain Lua 5.4. No custom DSL.
- Each label is a Lua function in a coroutine.
- Blocking calls (`say`, `menu`, `pause`, `wait`) yield to the engine. Scripts read top to bottom.
- Curated standard library: no raw `io` or `os` file access. Use the VFS API.
- The deterministic engine RNG replaces `math.random`.

### Example

```lua
local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }

label("start", function()
  scene("bg/classroom", { with = "dissolve" })
  show("eileen", { outfit = "school", face = "smile", at = "center" })
  say(eileen, "You came back.")

  local c = menu {
    "Of course I did.",
    "I forgot my bag.",
  }

  if c == 1 then
    store.affection = store.affection + 1
    jump("warm")
  else
    jump("cold")
  end
end)
```

### Core API (indicative)

- **Flow:** `label`, `jump`, `call`, `ret`, `menu`, `pause`, `wait`, `wait_all`.
- **Dialogue:** `character`, `say`, `narrate`, `nvl` mode, text tags (color, speed, pause, ruby).
- **Visuals:** `scene`, `show`, `hide`, `tween`, `transition`, `camera`, `layer`.
- **Audio:** `music`, `sound`, `voice`, `stop`, channel volumes.
- **Video:** `movie` (fullscreen or as a displayable).
- **3D:** `scene3d`, `model`, `play`, `camera(scene)`.
- **State:** `store`, `persistent`, `rand`.
- **UI:** `screen`, `call_screen`, `show_screen`, `hide_screen`, `bind`.
- **Platform:** `achievement`, `stat`, `rich_presence`.

## 8. State, Saves, Rollback

### Rules

- All game state is in `store`, an engine-owned Lua table. Dev builds warn on new globals outside `store`.
- `persistent` holds cross-playthrough data: seen text, unlocks, gallery, preferences. Rollback never changes it.
- Scene state (shown displayables, playing audio, active screens, camera) is engine-side. Snapshots include it with `store`.

### Position and replay

- Script position is a label name and a step count. A step is each yield point (`say`, `menu`, `pause`, etc.).
- On label entry, the engine snapshots `store` and scene state. It starts an input log (menu choices, input results).
- To reach a position, the engine restores the label-entry snapshot and **replays** the label silently to step N.
  - `say` returns at once.
  - Menus return the logged choices.
  - Audio and visuals stay off until the target step.
- RNG state is in `store`. All side effects use the engine API.

### Rollback

- The player can move back one step at a time, past menus too, and choose again.
- A menu can be set to a fixed choice.
- A ring buffer keeps label-entry snapshots and input logs. Default depth: 100 steps, configurable.
- Across a label boundary, the engine restores the previous label snapshot and replays.

### Saves

- Format: zstd-compressed MessagePack.
- Contents: engine version, game version, label, step, label source hash, label-entry snapshot, input log, persistent pointer, thumbnail, timestamp, user-facing title.
- **Script-edit safety:** If the label source hash changed, the game starts that label from its beginning. It shows a dev warning.
- **Migrations:** The `on_load(old_version, store)` hook renames or converts variables between releases.
- Slots, quick save, and auto save on configurable events.

### Backlog

- Read-only, scrollable dialogue history with voice replay. It uses the same step log as rollback, but it is separate from rollback.

### Skip / auto

- Skip seen text, using the persistent seen-step set.
- Skip all.
- Auto-advance with per-character timing and a voice-aware delay.

## 9. Rendering

- SDL3 GPU backend behind `IRenderer`: textured quads, batched sprites, render targets, custom shaders, depth buffer for 3D layers.
- A later WebGPU or WebGL backend serves the web build. It uses the same interface.
- Virtual resolution. Scaling modes: fit, fill, integer scale. Letterboxing and DPI awareness.
- Layers: background, master, characters, 3D, overlay, UI, transient. Ordered, each with its own shading.
- Custom HLSL shaders apply per displayable or per layer. Lua can tween shader uniforms.
- The asset pipeline builds texture atlases. Large backgrounds stream.

## 10. Animation

One timeline system controls 2D, UI, puppets, 3D, camera, and audio fades.

- **Tweens** change any displayable property: position, anchor, scale, rotation, alpha, tint, crop, shader uniforms.
- Easing library, sequence, parallel, loop, repeat, yoyo, and delays.
- Script can wait with `wait("eileen")` or `wait_all()`, or run a tween without waiting.
- Event hooks sync to text reveal, voice markers, or named timeline markers.
- **Transitions:** dissolve, fade, slide, wipes, image-mask transitions, custom shader transitions.
- **Frame animation:** sprite sheets and image sequences.
- **Layered characters:** named layers (base, outfit, face, accessories). Attributes compose the layers. An attribute change can crossfade.
- **Camera:** pan, zoom, rotate, shake, parallax depth per layer.
- **Lip sync:** amplitude-based mouth flaps from voice audio. Layered sprites, Inochi2D, and 3D morph targets can use them.

```lua
show("eileen", { at = "left", alpha = 0 })
tween("eileen", { x = 0.5, alpha = 1 }, 0.6, "ease_out")
shake("camera", { strength = 8, time = 0.3 })
wait_all()
```

## 11. Rigged 2D Puppets (Inochi2D)

- Inochi2D puppets load as displayables.
- The timeline tweens these parameters: head angle, eyes, mouth, breathing, custom parameters.
- Built-in idle behaviors: blink, breathe, optional sway.
- Lip sync binds to a mouth parameter.
- Expressions are named parameter presets.

## 12. 3D Support

3D support covers playback and staging only.

- **Format:** glTF 2.0 (`.glb` / `.gltf`).
- **Playback:** skeletal animation, morph targets, multiple named clips per model.
- **Timing:** play, crossfade, speed, seek, loop, await. Clips can sync to text or voice events through the timeline.
- **Compositing:** a 3D scene renders into a layer and mixes with 2D. Examples: 3D background with 2D sprites, 3D character with a painted background.
- **Camera:** scriptable position, look-at, FOV. All tweenable.
- **Lighting and shading:** directional and point lights. Unlit, toon, simple PBR. Custom shaders allowed.
- Excluded: physics, IK, model editing, real-time 3D gameplay.

```lua
local hall = scene3d("hall.glb")
model("kai", "kai.glb", { scene = hall, at = {0, 0, 2} })
play("kai", "wave", { fade = 0.3 })
camera(hall):tween({ fov = 35 }, 1.2, "ease_in_out")
wait("kai")
```

## 13. Text

- **Shaping:** HarfBuzz. **Rasterizing:** FreeType. **Line breaking:** libunibreak, with Japanese kinsoku rules. **Bidi:** SheenBidi.
- The glyph atlas is rasterized at actual screen resolution and rebuilt on resize.
- SDF glyphs are used only for effect text: outline, glow, shadow, large animated titles.
- Font fallback chains per language.
- Ruby text (furigana). Vertical text is a later addition.
- Typewriter reveal per character, with speed tags and inline pauses.
- Per-character effects: wave, shake, fade-in.
- Inline tags: color, size, font, bold/italic, speed, pause, ruby, link.

## 14. Audio and Video

- **Audio:** miniaudio. Named channels: music, sound, voice, ambient, UI, custom. Crossfades, loops with loop points, ducking music under voice, per-channel volume in preferences.
- Formats: Opus (recommended), Vorbis, WAV, FLAC.
- **Video:** AV1 in WebM via dav1d and libwebm, Opus audio track. Fullscreen cutscenes, or video as a displayable (looping backgrounds, video sprites). Hardware decode later, where available.
- The documentation gives a recommended encoding preset for authors.

## 15. UI System (Screens)

- A retained, declarative tree in Lua tables.
- Flexbox layout via Yoga.
- Styles and themes are style sheets in Lua tables. They inherit values. Each language can override them.
- Data binds to `store`, `persistent`, and preferences. The tree updates when bound data changes.
- Nodes: box, hbox, vbox, grid, text, image, button, toggle, slider, bar, input, viewport (scroll), any displayable.
- Every node can tween and has enter and exit transitions.
- Built-in focus navigation for keyboard and gamepad.
- Each node exposes accessibility data for the accessibility tree: role, label, state.
- The project template ships these default screens: title, say box, choice menu, NVL, save/load, preferences, backlog, gallery, confirm dialogs, quick menu.

```lua
screen "quick_menu" {
  hbox { align = "bottom", gap = 12,
    button { "Save", on_click = open("save") },
    button { "Auto", toggle = bind(prefs, "auto") },
    button { "Log",  on_click = open("backlog") },
  }
}
```

## 16. Localization

- `vnmaki extract-strings` collects every `say`, `narrate`, menu, and UI string into translation files.
- Format: gettext `.po`, which Poedit, Weblate, and Crowdin support. Entry keys are the label and step hash. Context is the original text and speaker.
- A missing translation uses the source language. The tool flags a changed source line as fuzzy.
- Per-language fonts, styles, text direction, and layout overrides.
- Runtime language switching.
- Localized images and voice use a path convention: `images/ja/...`, `voice/ja/...`.
- The dev tools include a translation editor: source and target side by side, live in-game preview.

## 17. Accessibility

- Screen reader support uses the UI accessibility tree (AccessKit).
- Self-voicing uses platform TTS: SAPI, AVSpeech, speech-dispatcher.
- Text scaling, high contrast theme, dyslexia-friendly font option.
- Reduce motion disables shake, flashes, large camera moves.
- Captions for sound effects and music cues.
- Full keyboard and gamepad operation. Remappable input.
- Adjustable text speed, auto-advance timing, window opacity.

## 18. Developer Tools (dev builds only)

- **Hot reload:** A file watcher reloads changed scripts, images, styles, shaders. After a script reload, the engine replays the current label to the current step.
- **Debugger:** Lua debugging through the Debug Adapter Protocol (VS Code and other DAP clients). Breakpoints, step in, step over, step out, call stack, locals, `store` inspection, watch expressions. Jumps to any label and step with replay.
- **Dev overlay (ImGui):** variable viewer and editor, scene graph inspector, displayable properties, active tweens, audio channels, performance stats, log console with Lua REPL.
- **Story graph:** static analysis of scripts (labels, `jump`, `call`, `menu` branches) drawn as a node graph. Edges found only at run time come from traces. Clicking a node opens its script or jumps there in-game. Shows unreachable labels and dead ends.
- **Screen editor (later):** WYSIWYG editing of the screen tree, written back to the Lua screen files.
- **Profiler:** frame timing, draw calls, texture memory, load times. Exports CSV.

## 19. Asset Pipeline and Packaging

- **Dev mode:** loose files, imported into a local cache when they change.
- **Import:**
  - Images: KTX2 (Basis Universal), atlas packing for small sprites.
  - Audio: normalization and encoding.
  - Fonts: subset per language.
  - glTF: validation and optimization.
- **Release:** `.vnpak` archives (indexed, zstd-compressed). Optional per-file encryption key for casual asset protection. Patch packs override base packs by path.
- **CLI:**
  - `vnmaki new <name>` — create project from template
  - `vnmaki run` — run in dev mode
  - `vnmaki build <platform>` — import + package
  - `vnmaki extract-strings` — localization export
  - `vnmaki check` — lint scripts, missing assets, unreachable labels
- Project config `project.toml`: title, version, resolution, languages, platforms, plugins, build options.
- A launcher GUI built on the CLI comes later.

## 20. Plugin API

- **C++ plugins:** implement module interfaces: new displayables, shaders, platform services, file formats, Lua bindings. Statically linked by default (console-safe). Dynamic loading allowed on desktop.
- **Lua plugins:** packages in `plugins/` that add API functions, screens, transitions, or effects.
- Plugins declare engine version compatibility in a manifest.

## 21. Platform Services

- `IPlatformServices` interface: achievements, stats, cloud saves, rich presence, overlay hooks.
- Default backend: local only. Achievements and saves are stored locally.
- Optional modules for Steamworks, GOG Galaxy, console services. The public repo omits a module when its license forbids it.
- Script API is identical across backends.

## 22. Repository Layout

```
vnmaki/
  engine/
    core/ runtime/ state/ scene/ anim/ ui/ text/
    audio/ video/ model3d/ puppet/ render/ platform/
    vfs/ jobs/ tools/
  cli/
  shaders/
  templates/default/      # project template + default screens
  plugins/                # first-party optional plugins
  third_party/            # vcpkg overlays, vendored single-file libs
  tests/  fuzz/  bench/
  tools/generators/       # test fixture generators
  docs/
  CMakeLists.txt  CMakePresets.json  vcpkg.json
  .clang-tidy  .clang-format
  LICENSE (MIT)
```

## 23. Roadmap

1. **Foundation** — build system, CI, guardrails, window/input, renderer interface + SDL GPU backend, VFS, jobs.
2. **Core VN** — Lua runtime, labels/say/menu/jump, characters, images, text stack, audio, UI system + default screens, saves, rollback, backlog, skip/auto, preferences.
3. **Presentation** — timeline/tweens, transitions, layered sprites, camera, shaders, video, lip sync.
4. **Tools** — hot reload, DAP debugger, dev overlay, story graph, profiler, `vnmaki check`.
5. **Content features** — localization pipeline + translation editor, accessibility, asset pipeline + `.vnpak`, Inochi2D, 3D support, plugin API.
6. **Platforms** — macOS/Linux parity, web, Android/iOS, platform services, consoles.
7. **Later** — WYSIWYG screen editor, launcher GUI, vertical text.
