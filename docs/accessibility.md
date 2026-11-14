# Accessibility

The engine has these accessibility features (SPEC section 17). The player sets them on the `preferences` screen (the Prefs button of the quick menu, or Esc). They are the `prefs` keys of `persistent`, so a script can read and write them (`prefs.captions = true`) and saves and rollback do not change them. The rules for contributors are in `docs/CONVENTIONS.md` (Default screens and preferences, Runtime, Audio). The label `content` of the template shows captions and self-voicing.

| Feature | Preference | Default |
|---------|------------|---------|
| Text size | `text_scale` (0.75 to 2) | 1 |
| Dyslexia-friendly font | `dyslexia_font` | off |
| High contrast | `high_contrast` | off |
| Reduce motion | `reduce_motion` | off |
| Captions for sounds | `captions` | off |
| Self-voicing | `self_voicing` | off |
| Keys and buttons | `input_<action>` | see below |

## Text size

`text_scale` multiplies the size of the text of every screen and of the say box. Wrapped text reflows. Lengths that are not text sizes (widths, paddings) do not scale, so a screen of your own must use wrapping text and not fixed rows to fit at 2. The default `say` screen makes its box taller when the text is large.

## Dyslexia-friendly font

With `dyslexia_font` on, the font `fonts/dyslexia.otf` (or `fonts/dyslexia.ttf`) is the first of the font chain. The old font and the fallback fonts follow, for the characters that it lacks. A game without that file has no such font. The template has OpenDyslexic (SIL OFL 1.1); its license is in `licenses/`. OpenDyslexic has a Reserved Font Name, so `project.toml` lists it under `[build.fonts] keep` and the build copies it without a subset (`docs/build.md`). If you use another font for this, check its license for a Reserved Font Name and list it the same way.

## High contrast

With `high_contrast` on, the engine runs `styles/high_contrast.lua` over the styles of the game. A style that the file names keeps the options that it does not give, so the file changes colours and frames only. The template has black grounds, white text and frames, and a yellow focus. A game that has no such file gets one warning. A screen that is shown keeps its look until it is built again.

## Reduce motion

With `reduce_motion` on:

- A tween of `@camera` (and `camera { ... }`) is instant. The `wait` step stays, so a replay counts the same steps.
- `shake` does nothing.
- Every transition (`transition(...)` and `with = ...`) is a `dissolve` of at most 0.2 seconds. This includes a transition that a plugin defines with `define_transition`.

Other motion (tweens of pictures, animations, movies) does not change. A script can read the flag: `prefs.reduce_motion`.

## Captions for sounds

```lua
sound("sfx/chime", { caption = "a chime rings" })
music("bgm/theme", { fade = 1.5, caption = "soft music" })
ambient("bgm/night", { caption = "wind" })
```

- With `captions` on, the screen `captions` (top left, not modal) shows each caption as `[text]`. At most 4 show at once, the newest. The oldest go first.
- A sound's caption lasts as long as its file (3 seconds if the format does not say how long it is). The caption of music or `ambient` lasts 5 seconds, and only when the music changes.
- A replay (a load, a rollback) makes no caption.
- `vnmaki extract-strings` does not extract a caption (see `docs/localization.md`), so a caption shows as the script wrote it.
- A game can define its own screen `captions` (a function that gets a list of `{ text }`).

## Self-voicing

Self-voicing speaks the story and the interface for a player who cannot see the screen. It uses a text-to-speech voice: SAPI on Windows. Other systems have a backend that speaks nothing. A computer with no voice installed speaks nothing too.

The player turns it on with the switch "Self-voicing" on the preferences screen, or with the key `V` (the action `self_voicing`; it is on the Controls screen). When it turns off, it says "Self-voicing off". A script can set `prefs.self_voicing`.

What it speaks:

- A new `say` line as `Speaker: text`, and a `narrate` line as the text. Text tags are removed.
- A menu once, as `Choose. First option. Second option`.
- The node that gets the focus, as `label, role` and the state: `, on` or `, off` for a toggle, `, 40 percent` for a slider or a bar, the text of an input, `, disabled`. A button's label is its text.
- The text is in the language of the player: the line, the options and the focused node are already translated (`docs/localization.md`).

The rules:

- A new item stops the speech that is still going.
- A line or a menu is spoken before the focus that came with it.
- Skip mode speaks nothing and stops the speech. What passes in skip mode is not spoken later.
- A rollback or a load that shows another line speaks it. When the story goes on without a new line, nothing is repeated.

A test can use a backend that keeps the strings (`tests/support/recording_tts.hpp`). `tests/unit/self_voicing_test.cpp` and `tests/unit/content_demo_test.cpp` do this.

## Keys, buttons and the keyboard

- The story and the screens work with the keyboard or a gamepad. Tab (or the west button of a gamepad) gives the keyboard to the top screen that is not modal, the quick menu. The arrow keys and the d-pad move the focus to the nearest button. Esc or the east button cancels or goes back. Enter, Space or the south button activates.
- The story actions have default keys and buttons: advance (Enter, Keypad Enter, Space, south), rollback (Page Up, left shoulder), auto (A), skip (S, right shoulder), backlog (L, north), menu (Esc, start) and self-voicing (V). The mouse is fixed: a click advances, the wheel up rolls back.
- The **Controls** screen (the Controls button of the preferences) lists them. A button starts a change, and the next key or gamepad button replaces the binding of that action. Esc and the east button cancel. Tab, Esc, the arrow keys, the d-pad and the east, west and guide buttons are reserved for the interface and cannot be bound. "Reset to defaults" restores them.
- The bindings are the preferences `input_<action>` (`key:enter key:space pad:south`).

## Not in the engine

A screen reader through AccessKit is not available: the C bindings need a Rust build and are not in vcpkg. Self-voicing replaces it until then. Each interface node has a role, a label and a state, so a screen reader can be added.
