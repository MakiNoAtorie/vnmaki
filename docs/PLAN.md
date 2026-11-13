# Remaining work plan

- Date: 2026-11-13
- Branch: `vnmaki/phase-1-2`
- PR: #21
- Specification: `docs/SPEC.md`

## Done

Tasks 01–09 and 11 are on `vnmaki/phase-1-2`:

- Build, CI, VFS, job system, window and input.
- Renderer, Lua runtime, text stack, say box.
- Scene graph, state store with replay.

## Phase 2

### Order

1. Do 10, 12, 14, 16 and 17 in parallel. Start 10 first.
2. After 10, do 13 and 15.
3. After 10, 12, 14, 15 and 16, do 18.
4. After 18, do 19. Then merge PR #21.

Do not do these tasks in parallel, because they change the same files:

- 10, 13, 15, 18: UI and screen code.
- 12, 13, 14: state store and snapshots.
- 14, 16: auto-advance timing.

### 10: UI system and `menu` (SPEC §15, §7)

- [x] Yoga layout (vcpkg). Nodes: box, hbox, vbox, text, image, button.
- [x] `screen`, `show_screen`, `hide_screen`, `call_screen`. `call_screen` yields until the screen returns a value.
- [x] `menu{...}` yields and returns a 1-based index.
- [x] Keys, d-pad, Tab and mouse hover move the focus. Each node has a role, a label and a state.
- [x] Make the say box a screen. The task 08 tests must pass.
- [x] Headless tests: node rectangles, focus order, menu choice.

### 12: Saves (SPEC §8)

- [x] MessagePack with zstd (vcpkg). Write saves to the user-data path, not to the VFS.
- [x] Fields: versions, label, step, source hash, snapshot, input log, persistent pointer, thumbnail, time, title.
- [x] Load through replay. If the source hash changed, go to the label start and show a dev warning.
- [x] Run `on_load(old_version, store)` before play continues.
- [x] Slots, quick save, quick load, auto save. `project.toml` sets the auto-save events.
- [x] Reject corrupt saves. Add a fuzz target for the loader.
- [x] Thumbnail from a renderer readback. Headless mode uses a stub.

### 13: Rollback (SPEC §8)

- [ ] Inputs: mouse wheel up, a key, a gamepad button.
- [ ] A ring buffer keeps snapshots and input logs. Default depth: 100 steps, set in `project.toml`.
- [ ] In a label, replay to step N-1. Across a label, restore the previous snapshot and replay.
- [ ] At a menu, the player can choose again. A new choice removes the forward history. `fixed = true` prevents this.
- [ ] Headless tests: in a label, across labels, across a menu, at the depth limit.

### 14: Persistent store, seen text, skip, auto (SPEC §8)

- [ ] `persistent` is a Lua table. Save it on change and on exit. Load it at start.
- [ ] Rollback and load do not change `persistent`.
- [ ] Seen steps use label, step and source hash as the key. An edited line is unseen.
- [ ] "Skip seen" stops at the first unseen line or menu. "Skip all" stops at menus.
- [ ] Auto delay = base + time for each character × the `character{}` multiplier. Add a hook for the voice delay.
- [ ] Script and input turn skip and auto on and off. Show an indicator.
- [ ] Headless tests: skip seen, skip all at a menu, auto timing with a fake clock.

### 15: Backlog (SPEC §8)

- [ ] An entry has speaker, text, label, step and an optional voice.
- [ ] Make entries from the rollback step log. They must be correct after load and rollback.
- [ ] Configurable length. A screen with a scroll viewport for keyboard and gamepad.
- [ ] Headless tests: contents after play, rollback, load.

### 16: Audio (SPEC §14, §7)

- [x] miniaudio, Opus, Vorbis (vcpkg). Stream Opus, Vorbis, WAV and FLAC from the VFS.
- [x] Channels with volume: music, sound, voice, ambient, ui, custom.
- [x] `music`, `sound`, `voice`, `stop(channel, { fade })`. Music crossfades and has loop points.
- [x] Music volume decreases while voice plays.
- [x] Music state is in the scene snapshot. After replay or load, one-shot sounds do not play.
- [ ] Backlog replays voice. Auto-advance waits for the voice end.
- [x] Null audio device in headless tests. Fuzz target for the decoders.

### 17: Text tags, bidi, font fallback (SPEC §13, §7)

- [x] Tags: color, size, font, bold, italic, speed, pause, ruby, link. Example: `"I {color=#f00}really{/color} mean it.{pause=0.5}"`.
- [x] Document the syntax. A malformed tag gives an error with the script line. Fuzz target for the parser.
- [x] SheenBidi orders RTL text. libunibreak and kinsoku rules break Japanese lines.
- [x] A font fallback chain for each language. SDF only for outline and shadow text.
- [x] Layout tests: each tag, RTL, kinsoku, fallback.

### 18: Preferences and default screens (SPEC §15, §17)

- [ ] `bind(table, key)` connects a node to `store`, `persistent` or `prefs` in two directions.
- [ ] Nodes: toggle, slider, bar, grid.
- [ ] Preferences in `persistent`: text speed, auto delay, volumes, fullscreen, skip mode, window opacity.
- [ ] Screens: title, say, choice, save, load, preferences, backlog, quick menu, confirm. NVL and gallery are placeholders.
- [ ] Mouse, keyboard and gamepad operate all screens. A game can replace a Lua screen by name.
- [ ] Headless tests: save and load through the screens, a preference change.

### 19: `vnmaki new` and template (SPEC §19)

This task is the Phase 2 acceptance demo.

- [ ] `vnmaki new <name>` copies `templates/default/` and fills in `project.toml`. It stops if the folder is not empty.
- [ ] Assets with an MIT, OFL or CC0 license, or from generator scripts.
- [ ] The sample is the §7 example: classroom, Eileen, a menu to `warm` and `cold`.
- [ ] End-to-end headless test on the three CI platforms: new, both branches, save, load, rollback.
- [ ] "Getting started" document for `new` and `run`.

### Not planned yet

Plan these SPEC items before Phase 2 ends:

- [ ] `pause(seconds)` step (§7, §8).
- [ ] NVL mode and gallery with unlocks (§7, §8, §15).
- [ ] UI styles and themes, UI `input` node (§15).
- [ ] WebP images through libwebp (§4).
- [ ] cppcheck or MSVC `/analyze` in CI (§5).
- [ ] vcpkg in the cloud container with a git asset cache.

## Phase 3: Presentation

Each task updates its module contract in `docs/CONVENTIONS.md`. GPU goldens: each task adds its own generator, `tools/generators/<area>_fixtures.py`, which imports the reference rasterizer from `render_fixtures.py`. Do not change `render_fixtures.py` beyond small exports.

### Order

1. Do 20 first.
2. After 20, do these two lanes in parallel:
   - Drawing lane: 24, then 21, then 23.
   - Displayable lane: 22, then 27, then 25, then 26.
3. After 21–27, do 28.

Do not do these tasks in parallel, because they change the same files:

- 20, 21, 23, 24: scene drawing (`Scene::draw`, layer targets).
- 20, 22, 25, 27: scene entries (`Scene::show`).
- 21, 24: `shaders/` and shader loading.
- 22, 26: `engine/scene/layered_sprite.*`.
- 25, 26: `engine/audio/`.

All tasks add Lua API to `script_runtime.cpp` and `SceneCommands`. Add new code at the end of each section.

### 20: Timeline and tweens (SPEC §10, §6, §8)

Files: new `engine/anim/`, `engine/scene/scene.*`, `engine/scene/displayable.*`, `engine/runtime/script_runtime.*`.

- [x] `Scene` keeps `std::unique_ptr<Displayable>`, not only images. The scene snapshot is a map with one key for each part, so later tasks can add keys.
- [x] `tween(tag, { x, y, anchor, scale, rotation, alpha, tint, crop, duration, ease, delay, loop, yoyo })` uses the same units as `show`. `sequence{...}` and `parallel{...}` group tweens. Easings: linear, plus sine, quad, cubic, back and bounce, each as `in`, `out` and `in_out`.
- [x] `wait(tag)` and `wait_all()` yield until those tweens end. A click or skip mode ends them at once. A tween that has no `wait` does not block.
- [x] Replay and load put each tween at its end value. A loop tween restarts at time 0. Active loop tweens are in the snapshot.
- [ ] Rollback goes back over `wait` steps to the previous `say` or `menu`. Not done: it needs task 13. Wait steps are marked with `ScriptPosition::wait` (from `ScriptObserver::on_blocked` and `position()`). Rollback must skip them.
- [x] Unit tests with a fake clock: each easing at t = 0, 0.5 and 1. Sequence, parallel, loop, yoyo and delay. Equal values for 1/60 s and 1/7 s steps. Snapshot round trip.

### 21: Transitions (SPEC §10, §9)

Files: new `engine/anim/transition.*`, `shaders/transition_*.hlsl`, `engine/scene/scene.cpp`, runtime.

- [ ] `with = name` on `scene`, `show` and `hide`, and `transition(name, options)`. All changes in one step use one transition.
- [ ] The scene draws the old frame and the new frame into two virtual-resolution targets. A transition shader mixes them. The UI and transient layers are not part of the transition.
- [ ] Built-in transitions: dissolve, fade through a color, slide and wipe (left, right, up, down), and image mask (`mask = "masks/x"`, `softness`). A custom shader from task 24 can be used by name.
- [ ] A transition blocks like `wait`, and a click ends it. Replay and skip mode do not show a transition. An unknown name gives an error at the script line.
- [ ] GPU goldens for each built-in transition at progress 0, 0.5 and 1, with a fake clock.

### 22: Layered sprites (SPEC §10, §7)

Files: new `engine/scene/layered_sprite.*`, `engine/scene/scene.cpp`, runtime.

- [x] `layered(name, { order = { "base", "outfit", "face" }, base = "eileen/base", outfit = { school = "..." }, face = { ... }, default = { ... }, crossfade = 0.2 })`.
- [x] `show("eileen", { outfit = "school", face = "smile" })` changes only the groups it names (the §7 example). An unknown group or value gives an error at the script line.
- [x] Layers draw in the declared order at the same origin. `bounds()` is the base image.
- [x] A changed group crossfades from the old image to the new image. Replay does not crossfade. Tweens, `with` and shaders work as they do on an image.
- [x] The attributes are in the snapshot.
- [x] Tests: unit tests for attribute merge, errors and snapshot. GPU goldens for a composed sprite and for the middle of a crossfade.

### 23: Camera and parallax (SPEC §10, §17)

Files: new `engine/scene/camera.*`, `engine/scene/scene.cpp` (draw), `engine/anim/`, runtime.

- [ ] `camera { x, y, zoom, rotation, duration, ease }` moves the view of the layers from background to overlay. The UI and transient layers do not move.
- [ ] `layer(name, { parallax = f })`: the pan of that layer is f × the camera pan. The default is 1.
- [ ] `shake { strength, duration }` uses a fixed noise function of time. It does not use `rand`, so `store` does not change.
- [ ] `prefs.reduce_motion` stops shake and makes camera moves instant.
- [ ] `tween("@camera", ...)` and `wait("@camera")` work. The camera is in the snapshot. After replay, the camera is at its end value.
- [ ] GPU goldens: pan, zoom, rotation, parallax at two depths, and one shake frame at a fixed time.

### 24: Custom shaders and uniform tweens (SPEC §9, §10)

Files: `engine/render/` (shader loading), `engine/scene/scene.cpp` (draw), `vcpkg.json`, runtime, `tests/render/shaders/`.

- [ ] Game shaders are VFS files, `shaders/<name>.frag.hlsl`, and include `vnmaki.hlsli`. Dev builds compile them at load with the SDL_shadercross library (vcpkg, also as a target dependency). A compile error gives the file, the line and the message.
- [ ] `shader(name, { "amount", ... })` declares the uniforms. They are floats, packed into `b0` in that order and padded to 16 bytes.
- [ ] `show(tag, { shader = name, uniforms = {...} })` sets a shader on a displayable. `layer(name, { shader = ... })` draws the layer into a target, then draws that target with the shader.
- [ ] `tween(tag, { uniforms = { amount = 1 } })` changes uniforms. Shader names and uniform values are in the snapshot.
- [ ] GPU goldens: a sepia shader on a displayable and on a layer, at two uniform values. A bad shader gives an error test.

### 25: Video (SPEC §14, §4, §5)

Files: new `engine/video/`, `engine/audio/` (movie stream), `vcpkg.json`, `engine/scene/scene.cpp`, runtime, `tests/fixtures/video/`.

- [ ] Use dav1d and libwebm (vcpkg). Read WebM from the VFS. Decode AV1 on a job thread. Upload frames on the main thread.
- [ ] The Opus audio of the movie plays on a custom `movie` channel. The picture follows the audio clock. If the movie has no audio, the picture follows the main loop clock.
- [ ] `movie(path, { skippable = true })` plays over all layers and blocks. Replay and skip mode do not play it.
- [ ] `show(tag, { movie = path, loop = true })` makes a movie displayable. After a load, it restarts at time 0.
- [ ] Add a fuzz target for the WebM and AV1 loader. A bad file gives an error at the script line.
- [ ] Fixture: Python cannot write AV1. Commit one small CC0 clip of solid-color frames, with the ffmpeg command that made it, and add this exception to CONVENTIONS. Tests: the color of frame N (with a tolerance), the duration, the loop, and sync with the null audio device.

### 26: Lip sync (SPEC §10, §14)

Files: `engine/audio/` (voice level), `engine/scene/layered_sprite.*`, runtime.

- [ ] The audio module gives the RMS level of the voice that plays for each `voice_tag`. This also works with the null audio device.
- [ ] `layered` accepts `lip_sync = { group = "mouth", frames = { "closed", "half", "open" }, thresholds = { 0.05, 0.2 } }`.
- [ ] While the character's voice plays, the group shows the frame for the level. When the voice stops, the group shows the first frame.
- [ ] Replay and skip mode play no voice, so the mouth stays closed. Lip sync state is not in the snapshot.
- [ ] A generator writes a WAV of silence, then a tone, then silence (Python `wave`). Unit test with a fake clock: the mouth is closed, then open, then closed at the expected times.

### 27: Frame animation (SPEC §10)

Files: new `engine/scene/frame_animation.*`, `engine/scene/scene.cpp`, runtime, a fixture generator.

- [ ] `animation(name, { frames = { "a", "b" }, fps, loop })` makes an animation from images. `animation(name, { sheet = "x", columns, rows, count, fps })` makes one from a sprite sheet.
- [ ] `show` accepts an animation name as it accepts an image name. A layered sprite group value can be an animation, for example a blink.
- [ ] `loop = false` holds the last frame, and `wait(tag)` waits for that last frame. After a load, the animation restarts at frame 1.
- [ ] Tests: the frame index at fixed times, and a GPU golden of one sheet frame.

### 28: Presentation demo (SPEC §22)

This task is the Phase 3 acceptance demo.

- [ ] Add a `presentation` label to `templates/default/`. It has a dissolve, a layered sprite with a crossfaded face change, a blink, a camera pan and shake, a sepia layer shader, a lip-synced voice line and a short movie.
- [ ] Generator scripts make all assets, except the movie (see task 25).
- [ ] End-to-end headless test on Windows: `new`, play `presentation`, save during a tween, load, and roll back over a transition.
- [ ] GPU goldens of the demo at three fixed frames.
- [ ] Write a script reference for the presentation API in `docs/`.

### Deferred

- Event hooks for text reveal, voice markers and timeline markers (§10): Lua callbacks between steps break replay. Design them with rollback first.
- Lip sync for puppets and 3D morph targets (§10, §11): Phase 5, using the voice level from task 26.
- Tweens and enter and exit transitions for UI nodes (§15): use tasks 20 and 21 once the UI is stable.
- Text effects for each character: wave, shake, fade-in (§13): plan with the text work.
- Precompiled game shaders for release builds (§19): Phase 5 asset pipeline.
- Hardware video decode (§14), streaming of large backgrounds (§9): performance pass.
- Depth buffer and the 3D layer (§9, §12): Phase 5.

## Phase 4: Tools

Dev tools are in dev builds only. The CMake option `VNMAKI_DEV_TOOLS` is ON in `debug` and OFF in `release`. All tool code is in `engine/tools/`. Each task updates its module contract in `docs/CONVENTIONS.md`. New Win32-only code has a null fallback, so the other CI jobs still build.

### Order

1. Do 30 and 34 in parallel.
2. After 30, do two lanes in parallel. Overlay lane: 31, then 33. Runtime lane: 32, then 36.
3. After 34, do 35.
4. After 30–36, do 37.

Do not do these tasks in parallel, because they change the same files:

- 30, 31, 33: `engine/tools/overlay.*` (panel list).
- 32, 36: `ScriptRuntime` reload, replay and Lua hooks.
- 34, 35: `engine/tools/script_scan.*`.

Tasks 30, 32, 33 and 36 add one hook call each to `cli/story_window.cpp`. Tasks 33–36 add commands to `cli/main.cpp`. Add new code at the end of each section.

### 30: Dev overlay, log console and Lua REPL (SPEC §18, §4)

Files: new `engine/tools/overlay.*`, `engine/tools/imgui_backend.cpp`, `vcpkg.json`, `CMakeLists.txt`, `cli/story_window.cpp`, `tests/platform/no_sdl_outside_platform.cmake`.

- [ ] Dear ImGui (vcpkg, with the SDL3 and SDL GPU backend features). Only `imgui_backend.cpp` includes SDL. Add it to the SDL allow-list.
- [ ] F12 shows and hides the overlay. The overlay draws in the screen-resolution pass, after the game. While it has the input focus, the game gets no input.
- [ ] A log console shows the engine log and Lua `print`. It has filters for each level.
- [ ] The REPL runs Lua in the script sandbox and prints the result or the error. A change to `store` stays until the next rollback or load. Document this.
- [ ] The `release` preset builds without ImGui and without `engine/tools/`. A CLI test checks that a release `vnmaki --version` works.
- [ ] Tests: unit tests for the REPL (result, error, sandbox), and a headless GPU smoke test that draws one overlay frame.

### 31: Inspectors (SPEC §18)

Files: new `engine/tools/inspect_*.cpp`, `engine/tools/overlay.cpp`.

- [ ] Panels: `store` and `persistent` as a tree, scene graph by layer, the properties of the selected displayable, active tweens, and audio channels (file, volume, position).
- [ ] The panel data comes from functions without ImGui (`inspect_store()` returns rows). ImGui only draws the rows.
- [ ] An edit of a displayable property applies at once. It is not in the snapshot. A note in the panel says this.
- [ ] A label list with a "Jump" button. It goes to step 1 of that label through the replay code.
- [ ] Unit tests for each row function with the task 19 template scene.

### 32: Hot reload (SPEC §18, §4)

Files: new `engine/tools/file_watcher.*`, `engine/tools/hot_reload.*`, `engine/runtime/script_runtime.*`, `engine/scene/`, `vcpkg.json`.

- [ ] efsw (vcpkg) watches the project folder. Events within 100 ms become one batch. A unit test can inject a batch with no efsw.
- [ ] A changed `.lua` file reloads all scripts (labels, screens, styles). Then the engine replays to the current label and step. If the label is gone, it goes to `start`. If the step is past the label end, it stops at the last step.
- [ ] A script error keeps the old scripts. The error and its line go to the log, and a red banner shows.
- [ ] A changed image replaces its texture. A changed game shader compiles again (task 24). A compile error keeps the old shader.
- [ ] Hot reload does not change `persistent` and does not make a rollback step.
- [ ] Tests: unit tests with injected batches (say text changes at the same step, label removed, syntax error). A Windows platform test with a real file write and a timeout.

### 33: Profiler (SPEC §18, §5)

Files: new `engine/tools/profiler.*`, `engine/render/` (texture memory), `engine/platform/main_loop.*`, `cli/`.

- [ ] Each frame records CPU time for events, update, prepare and draw, and the draw calls from `FrameStats`.
- [ ] The renderer counts texture and target bytes on create and destroy. Asset loads record the path and the time.
- [ ] An overlay panel shows a frame-time graph of the last 300 frames, the counters, and the slowest loads.
- [ ] `vnmaki run <dir> --frames N --profile out.csv` writes one CSV row per frame. A second file, `out_loads.csv`, has the loads.
- [ ] Tests: unit test with a fake clock (exact phase times), a CSV header and row-count CLI test, and a texture-byte count test.

### 34: Story graph (SPEC §18)

Files: new `engine/tools/script_scan.*`, `engine/tools/story_graph.*`, `engine/tools/overlay.cpp` (one panel), `cli/`.

- [ ] A small Lua tokenizer (strings, long brackets, comments) finds `label`, `jump`, `call`, `menu`, `say` and `show` calls with string-literal arguments, and their lines. It does not run the scripts.
- [ ] Edges: jump, call, and menu (a jump or call in the body of a menu branch). A computed target is a "dynamic" edge.
- [ ] Unreachable labels: no path from `start`. Dead ends: a label that ends with no transfer and is not marked `ending = true`.
- [ ] Run-time traces add the edges seen in play through `ScriptObserver`. The overlay panel shows them in a second color.
- [ ] `vnmaki graph <dir> [--format dot|csv]` prints the graph.
- [ ] Tests: tokenizer cases (strings in comments, long strings), graph CSV for a generated fixture project, and a fuzz target for the tokenizer.

### 35: `vnmaki check` (SPEC §19, §18)

Files: new `engine/tools/check.*`, `cli/`, `tools/generators/check_fixtures.py`.

- [ ] Output is CSV: `file,line,severity,code,message`. The exit code is 1 if there is an error.
- [ ] Errors: Lua syntax, a jump or call to an unknown label, a malformed text tag (task 17 parser), a missing asset for a literal name in `scene`, `show`, `music`, `sound`, `voice` or `movie`.
- [ ] Warnings: unreachable labels, dead ends, dynamic edges, `project.toml` errors.
- [ ] The template project from task 19 gives no findings.
- [ ] Tests: one CLI stdout-file test for each finding code, with a generated fixture project.

### 36: Debugger (DAP) (SPEC §18)

Files: new `engine/tools/debugger.*`, `engine/tools/dap.*`, `engine/runtime/script_runtime.*`, `cli/`, `vcpkg.json`.

- [ ] `vnmaki debug <dir>` speaks the Debug Adapter Protocol on stdin and stdout and opens the game window. nlohmann-json (vcpkg) reads and writes the messages.
- [ ] Requests: initialize, launch, setBreakpoints (file and line), configurationDone, threads, stackTrace, scopes (locals, `store`), variables, evaluate (watches), continue, next, stepIn, stepOut, pause, disconnect.
- [ ] The custom request `vnmaki/goto { label, step }` goes to that position through replay.
- [ ] Breakpoints use a Lua line hook. Only this module uses `lua_sethook` and `lua_getlocal`. Add this exception to CONVENTIONS.
- [ ] While stopped, the window draws and handles OS events. The step count does not change. Replay does not stop at breakpoints.
- [ ] Tests: a CLI test sends a fixed DAP message file and compares the responses (breakpoint hit, locals, `store` value, step over, goto).

### 37: Tools demo (SPEC §22)

This task is the Phase 4 acceptance demo.

- [ ] End-to-end headless test on Windows: start the template, change a say line on disk, and check the new text at the same step.
- [ ] A DAP session test on the template: a breakpoint in `start`, step, read `store.affection`, goto `warm`.
- [ ] `vnmaki check` and `vnmaki graph` on the template and on a broken fixture give the expected CSV.
- [ ] A profile CSV of 120 frames of the template.
- [ ] Write `docs/tools.md`: the overlay keys, hot reload rules, a VS Code `launch.json` for `vnmaki debug`, and the check codes.

### Deferred

- GPU timing queries in the profiler: performance pass.
- Remote debugging over TCP: stdio is sufficient for a local VS Code.
- Hot reload of fonts and audio that plays: restart the label to see them.

## Phase 5: Content features

### Order

1. Do these lanes in parallel:
   - Localization lane: 40, then 41.
   - Accessibility lane: 42, then 43.
   - Package lane: 44, then 45.
   - 3D lane: 46, then 47.
2. After 45, do 48.
3. After 40–48, do 49.

Do not do these tasks in parallel, because they change the same files:

- 41, 42: UI styles and font chains (`engine/ui/`, `engine/text/`).
- 44, 45, 48: `engine/vfs/` mounts and `cli/build_command.*`.
- 46, 47: `engine/model3d/`, and mesh drawing in `engine/render/`.
- 40, 44: the string scan in `engine/tools/script_scan.*` (44 reads it for font subsets).

### 40: String extraction and `.po` files (SPEC §16)

Files: new `engine/l10n/po.*`, `engine/l10n/extract.*`, `engine/tools/script_scan.*`, `cli/`.

- [ ] `vnmaki extract-strings <dir>` uses the task 34 scan to find literal strings in `say`, `narrate`, `menu` and screen text nodes. It writes `translations/template.pot` and updates `translations/<lang>.po` for each language in `project.toml`.
- [ ] Key (`msgctxt`): `<label>/<hash>`, with a hash of the speaker and the source text. Screen strings use `screen/<name>/<hash>`. A comment gives the source file, line and speaker.
- [ ] An update keeps translations. A removed key whose label has a new key at the same position moves to that key as `#, fuzzy`. Other removed keys become obsolete (`#~`).
- [ ] A non-literal string gives a `check` warning. A translation whose text tags do not parse gives a `check` error.
- [ ] Tests: `.po` round trip, fuzzy and obsolete cases, and a fuzz target for the `.po` reader.

### 41: Runtime languages (SPEC §16, §13, §15)

Files: `engine/l10n/`, `engine/runtime/`, `engine/vfs/` (language lookup), `engine/ui/`, `engine/tools/translation_editor.*`.

- [ ] `prefs.language` and `vnmaki run --lang ja`. A missing or fuzzy entry uses the source text.
- [ ] Image `N` in language `ja` is `images/ja/N` if it exists. Voice uses `voice/ja/...` the same way.
- [ ] `project.toml` sets the font chain, the style override file (`styles/<lang>.lua`) and the text direction for each language. RTL languages reverse hbox order.
- [ ] A language change at run time updates the current line, the backlog and the screens at once. It is not a rollback step.
- [ ] The overlay translation editor lists the entries of the current label. An edit updates the say box at once and saves the `.po` file.
- [ ] Tests: headless transcript in two languages, image and voice lookup, a switch during a line, and an RTL hbox rectangle test.

### 42: Accessibility options (SPEC §17)

Files: `engine/ui/`, `engine/text/`, `engine/audio/`, `engine/platform/input.*`, `templates/default/`.

- [ ] `prefs.text_scale` (0.75–2.0) scales the say box and UI text, and the layout reflows.
- [ ] `prefs.high_contrast` loads `styles/high_contrast.lua`. `prefs.dyslexia_font` puts `fonts/dyslexia.ttf` (an OFL font in the template) first in the chain.
- [ ] `prefs.reduce_motion` also changes all transitions to a short dissolve (task 23 does shake and camera).
- [ ] `sound`, `music` and `ambient` accept `caption = "..."`. `prefs.captions` shows them in a caption screen for the sound length.
- [ ] Input actions (advance, rollback, skip, menu, ...) map to keys and gamepad buttons. A "Controls" screen changes the map, which is saved in `persistent`.
- [ ] Headless tests for each preference, and a remap test with injected input.

### 43: Self-voicing (SPEC §17)

Files: new `engine/platform/tts.hpp`, `engine/platform/win_tts.cpp`, `engine/dialogue/`, `engine/ui/`.

- [ ] Windows TTS through SAPI (`ISpVoice`, COM). No new dependency. Other platforms get a null backend.
- [ ] `prefs.self_voicing` and the V key. It speaks each say line (speaker, then text without tags), the menu choices, and the role, label and state of the focused UI node.
- [ ] New speech stops the old speech. Skip mode and replay do not speak.
- [ ] Tests: a recording fake backend gives the exact spoken strings for a scripted session. A Windows platform test speaks one line with a timeout.

### 44: Asset import (SPEC §19, §9)

Files: new `engine/assets/`, `vcpkg.json`, `cli/`.

- [ ] The import cache is `.vnmaki/cache/`. The key is a hash of the source file, the importer name and its version. Dev mode imports changed files through the task 32 watcher.
- [ ] Audio: WAV and FLAC to Opus (libopusenc, vcpkg), with a peak or RMS gain to the target in `project.toml`.
- [ ] Fonts: a subset for each language with the characters of its strings plus ASCII (HarfBuzz subset).
- [ ] glTF: validation with cgltf (vcpkg). Missing textures and buffers are errors.
- [ ] Game shaders: precompile to SPIR-V and DXIL. Release builds load these and do not link SDL_shadercross.
- [ ] Tests: cache hit and miss, the gain of a generated WAV, glyph count of a subset font, and a bad glTF error.

### 45: `.vnpak` and `vnmaki build windows` (SPEC §19, §5)

Files: new `engine/vfs/pak_mount.*`, `engine/vfs/pak_writer.*`, new `cli/build_command.*`, `vcpkg.json`.

- [ ] Format: magic, version, a sorted index (path, offset, size, raw size, hash), and zstd data for each file. Document it.
- [ ] `PakMount` is a `VfsMount`. `patch-*.vnpak` files mount after `game.vnpak` in name order and override by path.
- [ ] `vnmaki build windows <dir> --out <folder>` imports, packs, and copies the release runner as `<title>.exe`. The runner plays `game.vnpak` next to it.
- [ ] A bad pak gives an error, not a crash. Add a fuzz target for the pak reader.
- [ ] Tests: pack round trip, patch override, and a headless play of the built template from packs only.

### 46: 3D scenes (SPEC §12, §9)

Files: new `engine/model3d/`, `engine/render/` (depth targets, meshes), `shaders/mesh_*.hlsl`, runtime, `tools/generators/model_fixtures.py`.

- [ ] cgltf loads glTF 2.0 (meshes, materials, textures) from the VFS. Add a fuzz target.
- [ ] `scene3d(name, { layer = "3d" })` renders into a target with a depth buffer. The 3D layer mixes with 2D layers.
- [ ] `model(tag, path, { scene, position, rotation, scale })`, `light(...)` (directional, point), and `camera(scene, { position, look_at, fov })`. Tweens and `wait` work on them.
- [ ] Shaders: unlit, toon, simple PBR, and custom (task 24).
- [ ] The 3D state is in the snapshot.
- [ ] Tests: the generator writes a cube glTF with Python `json` and `struct`. GPU golden for unlit. Pixel probes with a tolerance for toon and PBR.

### 47: 3D animation (SPEC §12, §10)

Files: `engine/model3d/`, `engine/audio/` (voice level from task 26), runtime.

- [ ] Skinning and morph targets on the CPU. Upload the vertices each frame.
- [ ] `play(tag, clip, { loop, fade, speed, seek })`. A fade crossfades from the old clip. `wait(tag)` waits for a clip with no loop.
- [ ] Replay puts a clip at its end. A loop clip restarts at time 0. The clip state is in the snapshot.
- [ ] `model(..., { lip_sync = { morph = "mouth_open" } })` uses the voice level of the model's `voice_tag`.
- [ ] Tests with a fake clock: joint matrices of a generated two-bone fixture, morph weights, crossfade weights, and one GPU probe.

### 48: Plugin API (SPEC §20)

Files: new `engine/plugin/`, `cmake/VnmakiPlugins.cmake`, runtime, `plugins/example/`.

- [ ] C++ plugins are static libraries. `vnmaki_add_plugin(name SOURCES ...)` registers one. Build a runner with plugins through `-DVNMAKI_PLUGINS=...`.
- [ ] A plugin gets a `PluginContext`: register a displayable type, a file format loader, and Lua functions. Public headers do not include sol2.
- [ ] Lua plugins are `plugins/<name>/` with `plugin.toml` and `.lua` files. They load before `scripts/` and can add API functions, screens, transitions and effects.
- [ ] `plugin.toml` declares the engine version range. A mismatch stops the start with a clear error. `project.toml` lists the active plugins.
- [ ] Tests: an example C++ plugin displayable (GPU golden), an example Lua plugin transition, and a version mismatch CLI test.

### 49: Content demo (SPEC §22)

This task is the Phase 5 acceptance demo.

- [ ] Add a `content` label to `templates/default/`. It has a generated Japanese `.po`, a language switch, captions, a 3D model with a clip, and a Lua plugin transition.
- [ ] End-to-end headless test on Windows: `build windows`, then play `content` from packs in both languages with the fake TTS.
- [ ] `vnmaki check` on the template gives no findings.
- [ ] GPU goldens of the demo at three fixed frames.
- [ ] Write `docs/localization.md`, `docs/accessibility.md`, `docs/plugins.md` and `docs/build.md`.

### Deferred

- Inochi2D puppets (§11): the reference runtime is in D, and Inox2D is in Rust. Neither is in vcpkg, and there is no C++ runtime. Plan it when a C or C++ runtime is in vcpkg, or when the owner accepts a subset port.
- AccessKit screen reader support (§17): the C bindings need a Rust build and are not in vcpkg. Task 43 self-voicing helps until then.
- KTX2, Basis Universal and atlas packing (§19, §4): performance pass.
- Per-file pak encryption (§19): the key must be in the executable, so it only deters. Add it on request.
- Dynamic plugin DLLs (§20): they need a C ABI. Static plugins are sufficient for now.

## Phase 6: Platforms

Out of scope for now. The owner targets Windows first.

## Phase 7: Later

### Order

1. Do 50, 51 and 52 in parallel.
2. After 50–52, do 53.

Do not do these tasks in parallel, because they change the same files:

- 50, 51: `engine/tools/overlay.*` and the ImGui window code.

### 50: Screen editor (SPEC §18, §15)

Files: new `engine/tools/screen_editor.*`, `engine/tools/script_scan.*`.

- [ ] An overlay mode: click a node in a screen that shows to select it. Its outline and Yoga rectangle show.
- [ ] The editor changes only literal number, string and boolean fields that are already in the screen table (`gap`, `align`, `text`, style values).
- [ ] The scan records the source span of each literal. A save replaces only those bytes. All other bytes and comments stay the same.
- [ ] After a save, hot reload (task 32) shows the change. A field with a computed value is read-only.
- [ ] Tests: span rewrite on a generated screen file (byte-exact diff), and a computed field that is read-only.

### 51: Launcher (SPEC §19)

Files: new `cli/launcher.*`, `cli/main.cpp`.

- [ ] `vnmaki launcher`, or `vnmaki` with no arguments, opens an ImGui window.
- [ ] It lists recent projects (saved in the user-data path). Buttons: New, Run, Check, Extract strings, Build windows, Open folder.
- [ ] Each button calls the same function as the CLI command. Command output shows in a log panel.
- [ ] A release runner next to `game.vnpak` does not open the launcher.
- [ ] Tests: unit tests for the recent-project list and the action functions, without ImGui.

### 52: Vertical text (SPEC §13, §16)

Files: `engine/text/`, `engine/dialogue/say_box.*`, `tools/generators/text_fixtures.py`.

- [ ] `layout_text` has a vertical mode: HarfBuzz top-to-bottom direction with the `vert` feature. Columns go from right to left.
- [ ] Latin runs turn 90 degrees. Kinsoku rules apply to columns. Ruby goes to the right of the column.
- [ ] `project.toml` and `styles/<lang>.lua` can set `vertical = true` for the say box and NVL.
- [ ] Tests: glyph positions with the box font, kinsoku at a column end, and one GPU golden of a vertical say box.

### 53: Later demo (SPEC §22)

This task is the Phase 7 acceptance demo.

- [ ] A headless test edits the template quick menu `gap` through the editor functions and checks the new node rectangles after reload.
- [ ] A launcher action test runs New, Check and Build windows on a temporary folder.
- [ ] A GPU golden of the template say box in vertical Japanese.
- [ ] Update `docs/tools.md` for the editor and the launcher.

### Deferred

- Add, remove and move nodes in the screen editor: this needs a Lua writer that keeps the format.
- Vertical text in UI nodes other than text, and mixed vertical and horizontal layouts.
