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

- [ ] Yoga layout (vcpkg). Nodes: box, hbox, vbox, text, image, button.
- [ ] `screen`, `show_screen`, `hide_screen`, `call_screen`. `call_screen` yields until the screen returns a value.
- [ ] `menu{...}` yields and returns a 1-based index.
- [ ] Keys, d-pad, Tab and mouse hover move the focus. Each node has a role, a label and a state.
- [ ] Make the say box a screen. The task 08 tests must pass.
- [ ] Headless tests: node rectangles, focus order, menu choice.

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

- [ ] Tags: color, size, font, bold, italic, speed, pause, ruby, link. Example: `"I {color=#f00}really{/color} mean it.{pause=0.5}"`.
- [ ] Document the syntax. A malformed tag gives an error with the script line. Fuzz target for the parser.
- [ ] SheenBidi orders RTL text. libunibreak and kinsoku rules break Japanese lines.
- [ ] A font fallback chain for each language. SDF only for outline and shadow text.
- [ ] Layout tests: each tag, RTL, kinsoku, fallback.

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

## Phases 3–7

See the SPEC §22 roadmap.
