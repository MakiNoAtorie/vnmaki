# VNMaki — Engine Specification

**Status:** Draft 1 — full product spec, to be split into versions later
**License:** MIT
**Purpose:** A public, open-source visual novel engine. Also the tool its author uses to build his own VN.

---

## 1. Goals

- A lean, fast, portable VN engine with a native core and no embedded heavy interpreter.
- Writer-friendly scripting in plain Lua, with full programmatic power when needed.
- Fix the main gaps people cite in Ren'Py: debugging, story overview, modern UI tooling, web/console portability, localization workflow, accessibility, asset pipeline, 3D and rigged-puppet support.
- Every bundled dependency is MIT-compatible. Proprietary SDKs (e.g. Steamworks, console SDKs) are bring-your-own optional modules.

## 2. Non-goals

- Ren'Py compatibility or `.rpy` import.
- Physics, IK, pathfinding, tile maps, or a real-time gameplay framework. (Minigames remain possible through Lua and plugins.)
- 3D modeling or animation authoring. 3D content is authored in external tools and played back.
- Live2D support.

---

## 3. Platforms

| Phase | Targets |
|---|---|
| First | Windows, Linux, macOS |
| Later | Web (Emscripten), Android, iOS |
| Final | Nintendo Switch, PlayStation, Xbox (via SDL console ports and platform SDKs, under NDA, out of the public repo) |

Design rules that keep the later targets possible from day one:
- No dynamic code loading required at runtime (consoles forbid it). Plugins can be statically linked.
- No JIT anywhere.
- All file access goes through the engine VFS (no direct `fopen` in game-facing code).
- Renderer and platform services sit behind thin interfaces.

---

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

---

## 5. C++ Guardrails

- Warnings as errors: `-Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror` (Clang/GCC), `/W4 /WX /permissive-` (MSVC).
- Build on at least Clang and MSVC.
- Hardened standard library in debug builds (`_GLIBCXX_ASSERTIONS` / libc++ hardening mode).
- clang-tidy: `bugprone-*`, `cppcoreguidelines-*`, `modernize-*`, `performance-*`, `concurrency-*`. cppcheck or MSVC `/analyze` as a second pass.
- clang-format enforced.
- ASan + UBSan on debug and CI builds; TSan on threaded subsystems.
- Fuzz targets for every loader of untrusted data: saves, scripts, packs, images, glTF, Inochi2D puppets, translation files.
- Code rules: RAII, no raw `new`/`delete`, `std::unique_ptr` by default, `std::span` / `std::string_view` over pointer + length, Lua C API used only through sol2 wrappers.
- CI (GitHub Actions): Windows/MSVC, Linux/Clang + sanitizers, macOS/Clang, clang-tidy, format check.
- Test fixtures are produced by generator scripts, not hand-made files. Benchmark and tool outputs are CSV.

---

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
- **model3d** — glTF loading, skinning, morph targets, 3D scenes.
- **puppet** — Inochi2D displayable.
- **render** — renderer interface + SDL GPU backend.
- **platform** — window, input, platform services (achievements, cloud saves).
- **vfs** — loose files in dev, `.vnpak` archives in release.
- **jobs** — thread pool for asset decoding and loading.
- **tools** — debugger, story graph, hot reload, inspectors (dev builds only).

**Displayable interface:** everything visible (images, layered sprites, text, video, Inochi2D puppets, 3D scenes, UI nodes, plugin content) implements one interface: update, draw, bounds, and tweenable properties. The animation system treats them all the same.

---

## 7. Scripting

### Model
- Plain Lua 5.4. No custom DSL.
- Each label is a Lua function run inside a coroutine. Blocking calls (`say`, `menu`, `pause`, `wait`) yield to the engine, so scripts read top to bottom.
- Game scripts get a curated stdlib: no raw `io`/`os` file access (use the VFS API), `math.random` replaced by the deterministic engine RNG.

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

---

## 8. State, Saves, Rollback

### Rules
- All game state lives in `store` (engine-owned Lua table). Writing new globals outside `store` warns in dev builds.
- Cross-playthrough data (seen text, unlocks, gallery, preferences) lives in `persistent` and is never rolled back.
- Scene state (shown displayables, playing audio, active screens, camera) is engine-side and snapshotted with `store`.

### Position and replay
- Script position = **label name + step count**. A step is each yield point (`say`, `menu`, `pause`, etc.).
- On label entry the engine snapshots `store` + scene state and starts an input log (menu choices, input results).
- To reach any position, the engine restores the label-entry snapshot and **replays** the label silently to step N: `say` returns immediately, menus return logged choices, audio and visuals are suppressed until the target step.
- Determinism: RNG state lives in `store`; all side effects go through the engine API.

### Rollback
- Player can scroll back step by step, including back past menus (re-choice allowed; configurable to fixed-choice per menu).
- Ring buffer of label-entry snapshots + input logs; depth configurable (default 100 steps).
- Rollback across a label boundary restores the previous label's snapshot and replays.

### Saves
- Format: zstd-compressed MessagePack.
- Contents: engine version, game version, label, step, label source hash, label-entry snapshot, input log, persistent pointer, thumbnail, timestamp, user-facing title.
- **Script-edit safety:** if a label's source hash changed since the save, load falls back to that label's start and shows a dev warning.
- **Migrations:** `on_load(old_version, store)` hook for renaming or converting variables between releases.
- Slots, quick save, auto save on configurable events.

### Backlog
- Read-only scrollable dialogue history with voice replay. Separate from rollback but built from the same step log.

### Skip / auto
- Skip seen text (via persistent seen-step set), skip all, auto-advance with per-character timing and voice-aware delay.

---

## 9. Rendering

- SDL3 GPU behind `IRenderer`: textured quads, batched sprites, render targets, custom shaders, depth buffer for 3D layers.
- Later backend: WebGPU/WebGL for the web build, slotted behind the same interface.
- Virtual resolution with scaling modes (fit, fill, integer scale), letterboxing, DPI awareness.
- Layer stack (background, master, characters, 3D, overlay, UI, transient), ordered and individually shaded.
- Custom shaders per displayable or per layer (HLSL), with uniforms tweenable from Lua.
- Texture atlases built by the asset pipeline; streaming of large backgrounds.

---

## 10. Animation

One timeline system drives everything (2D, UI, puppets, 3D, camera, audio fades).

- **Tweens** on any displayable property: position, anchor, scale, rotation, alpha, tint, crop, shader uniforms.
- Easing library, sequence / parallel / loop / repeat / yoyo, delays.
- Awaitable from script (`wait("eileen")`, `wait_all()`), or fire-and-forget.
- Event hooks: sync to text reveal, voice markers, or named timeline markers.
- **Transitions:** dissolve, fade, slide, wipes, image-mask transitions, custom shader transitions.
- **Frame animation:** sprite sheets and image sequences.
- **Layered characters:** named layers (base, outfit, face, accessories) composed by attributes; attribute changes can crossfade.
- **Camera:** pan, zoom, rotate, shake, parallax depth per layer.
- **Lip sync:** amplitude-based mouth flaps from voice audio, usable by layered sprites, Inochi2D, and 3D morph targets.

```lua
show("eileen", { at = "left", alpha = 0 })
tween("eileen", { x = 0.5, alpha = 1 }, 0.6, "ease_out")
shake("camera", { strength = 8, time = 0.3 })
wait_all()
```

---

## 11. Rigged 2D Puppets (Inochi2D)

- Inochi2D puppets load as displayables.
- Parameters (head angle, eyes, mouth, breathing, custom) are tweenable from the timeline.
- Built-in idle behaviors: blink, breathe, optional sway.
- Lip sync bound to a mouth parameter.
- Expressions as named parameter presets.

---

## 12. 3D Support

Playback and staging, not authoring.

- **Format:** glTF 2.0 (`.glb` / `.gltf`).
- **Playback:** skeletal animation, morph targets, multiple named clips per model.
- **Timing:** play, crossfade, speed, seek, loop, await. Clips can sync to text/voice events through the timeline.
- **Compositing:** a 3D scene renders into a layer and mixes freely with 2D (3D background + 2D sprites, 3D character + painted background, etc.).
- **Camera:** scriptable position, look-at, FOV, all tweenable.
- **Lighting and shading:** directional and point lights; unlit, toon, and simple PBR shading; custom shaders allowed.
- Excluded: physics, IK, model editing, real-time 3D gameplay.

```lua
local hall = scene3d("hall.glb")
model("kai", "kai.glb", { scene = hall, at = {0, 0, 2} })
play("kai", "wave", { fade = 0.3 })
camera(hall):tween({ fov = 35 }, 1.2, "ease_in_out")
wait("kai")
```

---

## 13. Text

- **Shaping:** HarfBuzz. **Rasterizing:** FreeType. **Line breaking:** libunibreak (including Japanese kinsoku rules). **Bidi:** SheenBidi.
- Glyph atlas rasterized at actual screen resolution, regenerated on resize, for maximum sharpness (especially small text and CJK).
- SDF glyphs only for effect text (outline, glow, shadow, large animated titles).
- Font fallback chains per language.
- Ruby text (furigana), vertical text as a later addition.
- Typewriter reveal per character with speed tags, inline pauses, and per-character animation effects (wave, shake, fade-in).
- Inline tags: color, size, font, bold/italic, speed, pause, ruby, link.

---

## 14. Audio and Video

- **Audio:** miniaudio. Named channels (music, sound, voice, ambient, UI, custom), crossfades, loops with loop points, ducking music under voice, per-channel volume in preferences.
- Formats: Opus (recommended), Vorbis, WAV, FLAC.
- **Video:** AV1 in WebM via dav1d + libwebm, Opus audio track. Fullscreen cutscenes or video as a displayable (looping backgrounds, video sprites). Hardware decode where available later.
- A recommended encoding preset is documented so authors can convert their videos.

---

## 15. UI System (Screens)

- Retained, declarative tree defined in Lua tables.
- Flexbox layout via Yoga.
- Styles and themes (style sheets as Lua tables), inheritance, per-language overrides.
- Data binding to `store`, `persistent`, and preferences; tree updates when bound data changes.
- Nodes: box, hbox, vbox, grid, text, image, button, toggle, slider, bar, input, viewport (scroll), any displayable.
- Every node is tweenable and supports enter/exit transitions.
- Focus navigation for keyboard and gamepad built in.
- Each node exposes accessibility info (role, label, state) for the accessibility tree.
- Default screen set shipped with the project template: title, say box, choice menu, NVL, save/load, preferences, backlog, gallery, confirm dialogs, quick menu.

```lua
screen "quick_menu" {
  hbox { align = "bottom", gap = 12,
    button { "Save", on_click = open("save") },
    button { "Auto", toggle = bind(prefs, "auto") },
    button { "Log",  on_click = open("backlog") },
  }
}
```

---

## 16. Localization

- `vnmaki extract-strings` collects every `say`/`narrate`/menu string and UI string into translation files.
- Format: gettext `.po` (works with Poedit, Weblate, Crowdin). Entries keyed by label + step hash, with the original text and speaker as context.
- Missing translations fall back to the source language; changed source lines are flagged as fuzzy.
- Per-language fonts, styles, text direction, and layout overrides.
- Runtime language switching.
- Localized images and voice by path convention (`images/ja/...`, `voice/ja/...`).
- Built-in translation editor in dev tools: side-by-side source/target, live preview in-game.

---

## 17. Accessibility

- Screen reader support via the UI accessibility tree (AccessKit).
- Self-voicing through platform TTS (SAPI, AVSpeech, speech-dispatcher).
- Text scaling, high contrast theme, dyslexia-friendly font option.
- Reduce motion (disables shake, flashes, large camera moves).
- Captions for sound effects and music cues.
- Full keyboard and gamepad operation; remappable input.
- Adjustable text speed, auto-advance timing, and window opacity.

---

## 18. Developer Tools (dev builds only)

- **Hot reload:** file watcher reloads changed scripts, images, styles, and shaders. After a script reload, the current label is re-entered via replay to the current step.
- **Debugger:** Lua debugging through the Debug Adapter Protocol (VS Code and other DAP clients): breakpoints, step in/over/out, call stack, locals, `store` inspection, watch expressions. Jump to any label/step using replay.
- **Dev overlay (ImGui):** variable viewer/editor, scene graph inspector, displayable properties, active tweens, audio channels, performance stats, log console with Lua REPL.
- **Story graph:** static analysis of scripts (labels, `jump`, `call`, `menu` branches) rendered as a node graph; edges found only at runtime are added from traces. Click a node to open the script or jump there in-game. Shows unreachable labels and dead ends.
- **Screen editor (later):** WYSIWYG editing of the screen tree, writing back to the Lua screen files.
- **Translation editor:** see §16.
- **Profiler:** frame timing, draw calls, texture memory, load times; exports CSV.

---

## 19. Asset Pipeline and Packaging

- **Dev mode:** loose files, imported into a local cache on change.
- **Import:** images → KTX2 (Basis Universal) with atlas packing for small sprites; audio normalized and encoded; fonts subset per language; glTF validated and optimized.
- **Release:** `.vnpak` archives (indexed, zstd-compressed, optional per-file encryption key for casual asset protection). Patch packs override base packs by path.
- **CLI:**
  - `vnmaki new <name>` — create project from template
  - `vnmaki run` — run in dev mode
  - `vnmaki build <platform>` — import + package
  - `vnmaki extract-strings` — localization export
  - `vnmaki check` — lint scripts, missing assets, unreachable labels
- Project config: `project.toml` (title, version, resolution, languages, platforms, plugins, build options).
- Launcher GUI built on the CLI comes later.

---

## 20. Plugin API

- **C++ plugins:** implement module interfaces (new displayables, shaders, platform services, file formats, Lua bindings). Statically linked by default (console-safe); dynamic loading allowed on desktop.
- **Lua plugins:** packages in `plugins/` that add API functions, screens, transitions, or effects.
- Plugins declare engine version compatibility in a manifest.

---

## 21. Platform Services

- `IPlatformServices` interface: achievements, stats, cloud saves, rich presence, overlay hooks.
- Default backend: local-only (achievements and saves stored locally).
- Optional bring-your-own-SDK modules: Steamworks, GOG Galaxy, console services. Not in the public repo where licensing forbids it.
- Script-side API is identical across backends.

---

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

---

## 23. Roadmap

1. **Foundation** — build system, CI, guardrails, window/input, renderer interface + SDL GPU backend, VFS, jobs.
2. **Core VN** — Lua runtime, labels/say/menu/jump, characters, images, text stack, audio, UI system + default screens, saves, rollback, backlog, skip/auto, preferences.
3. **Presentation** — timeline/tweens, transitions, layered sprites, camera, shaders, video, lip sync.
4. **Tools** — hot reload, DAP debugger, dev overlay, story graph, profiler, `vnmaki check`.
5. **Content features** — localization pipeline + translation editor, accessibility, asset pipeline + `.vnpak`, Inochi2D, 3D support, plugin API.
6. **Platforms** — macOS/Linux parity, web, Android/iOS, platform services, consoles.
7. **Later** — WYSIWYG screen editor, launcher GUI, vertical text.
