# Getting started

This guide makes a game with `vnmaki new` and plays it with `vnmaki run`.

## Make a game

```
vnmaki new my_game
```

- The command makes the folder `my_game` from the template `default`.
- The title of the game is the name of the folder. It is written to `project.toml`.
- The folder must not exist, or it must be empty. Otherwise the command stops and changes nothing.
- The command finds the template in `<folder of vnmaki>/templates/default`. If that folder does not exist, it uses `templates/default` of the source tree that built `vnmaki`.

The new game has these files:

| File | Content |
|------|---------|
| `project.toml` | Title, version, resolution (1280 by 720), languages, auto save. |
| `scripts/main.lua` | A short story: a classroom, Eileen, and a menu that leads to the labels `warm`, `cold` and `presentation`. |
| `scripts/presentation.lua` | The presentation demo (third menu option): dissolve, layered sprite, blink, camera, shake, sepia shader, lip sync, movie. See `docs/script-reference.md`. |
| `images/`, `audio/`, `shaders/`, `movies/`, `fonts/default.ttf` | Placeholder art, a music loop, a voice line, a shader, a short movie and a pixel font. |
| `ASSETS.txt` | The license of the assets (CC0). They come from `tools/generators/template_assets.py`. |

The font has only ASCII characters. Replace the assets with your own.

## Play the game

```
vnmaki run my_game
```

- The game opens in a window and starts at the title screen. Choose **Start**.
- Advance with Enter, Space, a click, or the south button of a gamepad.
- The quick menu (top right) opens the save, load and preferences screens.
- Roll back with the mouse wheel up or Page Up. `L` opens the backlog. `A` turns auto-advance on or off. `S` turns skip on or off.
- The game closes when the story ends.

Options:

- `vnmaki run` with no folder plays the game in the current folder.
- `--frames <n>` closes the window after `n` frames.
- `--data-dir <path>` gives the folder for saves and for the `persistent` data. Without it, they go to the user-data folder of the game title.
- `--headless` plays without a window and prints a transcript. It needs no input for lines, but it stops with an error at a menu or a screen. The sample story has both.
- `--lang <code>` plays in a language of `languages` in `project.toml` (see `docs/script-reference.md`, "Languages").

## Change the story

Open `scripts/main.lua`. Each `label` is a function. `say`, `narrate`, `menu` and `call_screen` wait for the player. `jump("name")` goes to another label. State that saves and rollback must keep goes in `store`. The full list of functions is in `docs/SPEC.md`, section 7.

## Build the game for players

```
vnmaki build windows my_game --out my_game_build
```

- The command imports the assets, packs them into `game.vnpak`, and copies the release runner as `<title>.exe` with its DLLs. Give the folder to a player. `<title>.exe` plays `game.vnpak` with no arguments.
- Run it with a dev build (it compiles the game shaders). The runner is `build/release/cli/vnmaki.exe`, so build the `release` preset too, or pass `--runner <path>`.
- A file `patch-*.vnpak` next to `game.vnpak` overrides the files of the game by path, in name order.
