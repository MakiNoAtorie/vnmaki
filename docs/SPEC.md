# VNMaki — Engine Specification

**Status:** Draft 1. The roadmap (§22) divides it into phases.
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
- 3D modeling or animation authoring. The engine only plays 3D content back.
- Live2D support.

## 3. Platforms

| Phase | Targets |
|---|---|
| First | Windows, Linux, macOS |
| Later | Web (Emscripten), Android, iOS |
| Final | Nintendo Switch, PlayStation, Xbox (SDL console ports, under NDA, out of the public repo) |

Design rules for the later targets:

- No dynamic code loading and no JIT at runtime. Consoles forbid them.
- Game code uses the VFS for all file access. It does not call `fopen`.
- The renderer and platform services use thin interfaces.

## 4. Tech Stack

| Area | Choice | License |
|---|---|---|
| Language | C++20 (C++23 where all target compilers support it) | — |
| Build | CMake presets, Ninja, ccache, vcpkg manifest mode | — |
| Window, input, gamepad, GPU | SDL3 and the SDL GPU API | zlib |
| Shaders | HLSL, compiled with SDL_shadercross | zlib |
| Scripting | Lua 5.4 (PUC) with sol2 | MIT |
| UI layout | Yoga (flexbox) | MIT |
| Text | HarfBuzz, FreeType, libunibreak, SheenBidi | MIT, FTL, zlib, Apache-2.0 |
| Audio | miniaudio, Opus, Vorbis | MIT-0, BSD |
| Video | AV1 via dav1d, WebM via libwebm | BSD |
| Images | stb_image (PNG/JPEG), libwebp, KTX2 + Basis Universal | MIT, BSD, Apache-2.0 |
| 3D models | glTF 2.0 via cgltf, glm | MIT |
| Rigged 2D puppets | Inochi2D (C API, or a native C++ runtime) | BSD-2 |
| Saves | MessagePack (own small codec), zstd | BSD |
| Config | TOML via toml++ | MIT |
| Dev tool UI | Dear ImGui | MIT |
| Accessibility tree | AccessKit (C bindings) | MIT/Apache |
| File watching | efsw | MIT |

## 5. C++ Guardrails

- Build, test and code rules are in `docs/CONVENTIONS.md`. Tools enforce them: warnings as errors, clang-tidy, clang-format, sanitizers, and a local CI script (`tools\dev\check.cmd`) on MSVC.
- Each loader of untrusted data has a fuzz target: saves, scripts, packs, images, glTF, Inochi2D puppets, translation files.
- MSVC `/analyze` runs on first-party code in the debug build of `tools\dev\check.cmd`. Later: cppcheck.
- Benchmark and tool output is CSV.

## 6. Architecture

Layers, top to bottom. A layer uses only the layers below it:

1. Game scripts (Lua) and dev tools (ImGui).
2. Script runtime, state, saves, rollback.
3. Scene, animation, UI, text, audio, video, 3D.
4. Renderer interface, platform interface, VFS, jobs.
5. SDL3 and the OS.

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
- **render** — renderer interface and SDL GPU backend.
- **platform** — window, input, platform services.
- **vfs** — loose files in dev, `.vnpak` archives in release.
- **jobs** — thread pool for asset decoders and loaders.
- **tools** — debugger, story graph, hot reload, inspectors (dev builds only).

**Displayable interface:** All visible items (images, layered sprites, text, video, puppets, 3D scenes, UI nodes, plugin content) implement one interface: update, draw, bounds, tweenable properties.

## 7. Scripting

- Plain Lua 5.4. No custom DSL.
- Each label is a Lua function in a coroutine.
- Blocking calls (`say`, `menu`, `pause`, `wait`) yield to the engine. Scripts read top to bottom.
- No raw `io` or `os` file access. Scripts use the VFS API.
- The deterministic engine RNG replaces `math.random`.

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

Core API (indicative):

- **Flow:** `label`, `jump`, `call`, `ret`, `menu`, `pause`, `wait`, `wait_all`.
- **Dialogue:** `character`, `say`, `narrate`, `nvl` mode, text tags.
- **Visuals:** `scene`, `show`, `hide`, `tween`, `transition`, `camera`, `layer`.
- **Audio and video:** `music`, `sound`, `voice`, `stop`, `movie`.
- **3D:** `scene3d`, `model`, `play`, `camera(scene)`.
- **State:** `store`, `persistent`, `rand`.
- **UI:** `screen`, `call_screen`, `show_screen`, `hide_screen`, `bind`.
- **Platform:** `achievement`, `stat`, `rich_presence`.

## 8. State, Saves, Rollback

### Rules

- All game state is in `store`, an engine-owned Lua table. Dev builds warn on new globals outside `store`.
- `persistent` holds cross-playthrough data: seen text, unlocks, gallery, preferences. Rollback and load never change it.
- Scene state (displayables, audio, screens, camera) is engine-side. Snapshots include it with `store`.
- RNG state is in `store`. All side effects use the engine API.

### Position and replay

- Script position is a label name and a step count. Each yield point is a step.
- On label entry, the engine snapshots `store` and scene state, and starts an input log.
- To reach a position, the engine restores the label-entry snapshot and replays the label silently to step N. `say` returns at once, menus return the logged choices, and audio and visuals stay off.

### Rollback

- The player can move back one step at a time, also past menus, and choose again. A menu can fix its choice.
- A ring buffer keeps label-entry snapshots and input logs. Default depth: 100 steps.
- Across a label boundary, the engine restores the previous label snapshot and replays.

### Saves

- Format: zstd-compressed MessagePack.
- Contents: engine and game versions, label, step, label source hash, label-entry snapshot, input log, persistent pointer, thumbnail, timestamp, title.
- If the label source hash changed, the game starts that label from its beginning and shows a dev warning.
- The `on_load(old_version, store)` hook migrates variables between releases.
- Slots, quick save, and auto save on configurable events.

### Backlog, skip, auto

- The backlog is a read-only dialogue history with voice replay. It uses the rollback step log.
- Skip seen text (from the persistent seen-step set) or skip all.
- Auto-advance with per-character timing and a voice-aware delay.

## 9. Rendering

- SDL GPU backend behind `IRenderer`: textured quads, batched sprites, render targets, custom shaders, depth buffer for 3D layers. A later WebGPU or WebGL backend serves the web build.
- Virtual resolution. Scaling modes: fit, fill, integer scale. Letterboxing and DPI awareness.
- Ordered layers: background, master, characters, 3D, overlay, UI, transient.
- Custom HLSL shaders apply per displayable or per layer. Lua can tween shader uniforms.
- Large backgrounds stream.

## 10. Animation

One timeline system controls 2D, UI, puppets, 3D, camera, and audio fades.

- **Tweens** change any displayable property: position, anchor, scale, rotation, alpha, tint, crop, shader uniforms. Easing, sequence, parallel, loop, yoyo, delays.
- A script can wait for a tween (`wait("eileen")`, `wait_all()`) or not.
- Event hooks sync to text reveal, voice markers, or timeline markers.
- **Transitions:** dissolve, fade, slide, wipes, image masks, custom shaders.
- **Frame animation:** sprite sheets and image sequences.
- **Layered characters:** attributes compose named layers (base, outfit, face, accessories). An attribute change can crossfade.
- **Camera:** pan, zoom, rotate, shake, parallax per layer.
- **Lip sync:** amplitude-based mouth flaps from voice audio, for layered sprites, puppets, and 3D morph targets.

## 11. Rigged 2D Puppets (Inochi2D)

- Puppets load as displayables. The timeline tweens their parameters.
- Built-in idle behaviors: blink, breathe, optional sway.
- Lip sync binds to a mouth parameter. Expressions are named parameter presets.

## 12. 3D Support

Playback and staging only.

- glTF 2.0 with skeletal animation, morph targets, and named clips.
- Clips play, crossfade, loop, seek, and sync to the timeline.
- A 3D scene renders into a layer and mixes with 2D.
- Tweenable camera: position, look-at, FOV.
- Directional and point lights. Unlit, toon, simple PBR, custom shaders.

## 13. Text

- Libraries: see §4. Line breaks obey Japanese kinsoku rules.
- SDF glyphs only for effect text: outline, glow, shadow, large animated titles.
- A font fallback chain for each language.
- Inline tags: color, size, font, bold, italic, speed, pause, ruby, link.
- Typewriter reveal. Effects per character: wave, shake, fade-in.
- Vertical text (Japanese and Chinese tategaki): columns run down and from right to left. Upright characters use the font's `vert` forms, Latin runs turn 90 degrees, kinsoku works on columns and ruby goes to the right. A language sets it in `project.toml` or its style file (section 16). It applies to the say box and NVL; other UI text and mixed layouts come later.

## 14. Audio and Video

- **Audio channels:** music, sound, voice, ambient, UI, custom. Each has a volume preference. Crossfades, loop points, music ducks under voice.
- Audio formats: Opus (recommended), Vorbis, WAV, FLAC.
- **Video:** AV1 in WebM with Opus audio. Fullscreen or as a displayable. Hardware decode later.

## 15. UI System (Screens)

- A retained, declarative tree in Lua tables, with Yoga flexbox layout.
- Style sheets are Lua tables. They inherit values. Each language can override them.
- Nodes bind to `store`, `persistent`, and preferences, and update when the data changes.
- Nodes: box, hbox, vbox, grid, text, image, button, toggle, slider, bar, input, viewport (scroll), any displayable.
- Every node can tween and has enter and exit transitions.
- Focus navigation for keyboard and gamepad. Each node has an accessibility role, label and state.
- The project template ships default screens: title, say box, choice menu, NVL, save/load, preferences, backlog, gallery, confirm, quick menu.

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

- `vnmaki extract-strings` writes all dialogue, menu, and UI strings to gettext `.po` files. The key is the label and step hash. The context is the source text and speaker.
- A missing translation uses the source language. A changed source line becomes fuzzy.
- Per-language fonts, styles, text direction, layout, images (`images/ja/...`) and voice (`voice/ja/...`).
- Runtime language switching.
- A dev-tools translation editor with live in-game preview.

## 17. Accessibility

- Screen reader support through the UI accessibility tree (AccessKit).
- Self-voicing through platform TTS.
- Text scaling, high contrast theme, dyslexia-friendly font.
- Reduce motion disables shake, flashes, and large camera moves.
- Captions for sound effects and music cues.
- Full keyboard and gamepad operation. Remappable input.
- Adjustable text speed, auto-advance timing, window opacity.

## 18. Developer Tools (dev builds only)

- **Hot reload** of scripts, images, styles and shaders. After a script reload, the engine replays to the current step.
- **Debugger** through the Debug Adapter Protocol: breakpoints, stepping, call stack, locals, `store`, watches, jump to any label and step.
- **Dev overlay (ImGui):** variables, scene graph, displayable properties, tweens, audio channels, performance, log console with Lua REPL.
- **Story graph** of labels and their `jump`, `call` and `menu` edges, from static analysis and run-time traces. It shows unreachable labels and dead ends.
- **Profiler:** frame timing, draw calls, texture memory, load times.
- **Screen editor (later):** WYSIWYG editing that writes back to the Lua screen files.

## 19. Asset Pipeline and Packaging

- **Dev mode:** loose files, imported into a local cache when they change.
- **Import:** images to KTX2 with atlas packing, audio normalization and encoding, font subsets per language, glTF validation.
- **Release:** indexed, zstd-compressed `.vnpak` archives. Optional per-file encryption. Patch packs override base packs by path.
- **CLI:**
  - `vnmaki new <name> [--lang <code>]...` — create a project from the template; `--lang` adds a language add-on (`templates/addons/<code>/`, e.g. `ja`)
  - `vnmaki run` — run in dev mode
  - `vnmaki import <dir>` — import the assets into the cache (`.vnmaki/cache/`)
  - `vnmaki build <platform>` — import and package
  - `vnmaki extract-strings` — localization export
  - `vnmaki check` — lint scripts, missing assets, unreachable labels
- `project.toml`: title, version, resolution, languages, platforms, plugins, build options.
- `vnmaki launcher` (or `vnmaki` with no arguments, in a dev build) — a window with recent projects and buttons for New, Run, Check, Extract strings, Build windows and Open folder.

## 20. Plugin API

- **C++ plugins** add displayables, shaders, platform services, file formats, or Lua bindings. Static linking by default. Dynamic loading only on desktop.
- **Lua plugins** in `plugins/` add API functions, screens, transitions, or effects.
- A plugin manifest declares engine version compatibility.

## 21. Platform Services

- `IPlatformServices`: achievements, stats, cloud saves, rich presence, overlay hooks.
- The default backend is local only.
- Optional modules for Steamworks, GOG Galaxy, and consoles. The public repo omits a module when its license forbids it.
- The script API is the same for all backends.

## 22. Roadmap

1. **Foundation** — build system, CI, guardrails, window and input, renderer, VFS, jobs.
2. **Core VN** — Lua runtime, labels, say, menu, characters, images, text, audio, UI and default screens, saves, rollback, backlog, skip, auto, preferences.
3. **Presentation** — timeline, tweens, transitions, layered sprites, camera, shaders, video, lip sync.
4. **Tools** — hot reload, debugger, dev overlay, story graph, profiler, `vnmaki check`.
5. **Content features** — localization, accessibility, asset pipeline, `.vnpak`, Inochi2D, 3D, plugin API.
6. **Platforms** — web, Android, iOS, platform services, consoles.
7. **Later** — WYSIWYG screen editor, launcher GUI, vertical text.
