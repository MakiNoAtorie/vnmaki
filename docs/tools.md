# Tools

This guide is for the person who makes a game. The tools help you to write, test and tune it.

| Tool | Command or key | Builds |
|------|----------------|--------|
| Dev overlay | `F12` in `vnmaki run` | Dev builds |
| Hot reload | Save a file while `vnmaki run` or `vnmaki debug` runs | Dev builds |
| Screen editor | The window "Screen editor" of the overlay | Dev builds |
| Debugger (DAP) | `vnmaki debug` | Dev builds, Windows |
| Launcher | `vnmaki launcher`, or `vnmaki` with no arguments | Dev builds |
| Check | `vnmaki check <dir>` | All builds |
| Story graph | `vnmaki graph <dir>` | All builds |
| Extract strings | `vnmaki extract-strings <dir>` | All builds |
| Profile CSV | `vnmaki run <dir> --frames N --profile out.csv` | All builds |

A dev build is a build with the CMake option `VNMAKI_DEV_TOOLS` (on for the `debug` preset, off for `release`). In a release build the overlay, hot reload, the launcher and `vnmaki debug` do not exist; `vnmaki debug` then prints "this build has no debugger" and exits with 1.

## Dev overlay

Press `F12` in a window to show or hide the overlay. While it is hidden it does no work, and the game gets the key events. While it is visible, the game gets no pointer event over an overlay window, and no key or text that a text field of the overlay wants. A click outside the windows goes to the game.

| Window | Content |
|--------|---------|
| Console | The log (the last 1000 entries). The check boxes Info, Warning and Error filter it, Clear empties it. Lua `print` is info. A dev-mode warning of the runtime, the saves and the persistent store is a warning. A hot reload error is an error. The input line at the bottom is the REPL (see below). |
| Inspectors | One window with six tabs: Store, Persistent, Scene, Tweens, Audio, Labels. |
| Story graph | The labels and edges of the scripts (red: not reachable from `start`, yellow: dead end). What the run took is green. Rescan reads the scripts again. |
| Screen editor | Select a node of a shown screen and change its literal values (see below). |
| Profiler | A graph of the CPU time of the last 300 frames, the phase times of the last frame, the GPU counters, the texture memory, and the 8 slowest asset loads. |

The REPL runs a line in the script sandbox, as a script does: the same globals and limits, and no `io`, `debug`, `package` or `require`. An expression prints its values. A blocking call (`say`, `menu`, `wait`, ...) is an error. A change to `store` stays until the next rollback or load. `persistent` and `prefs` keep their changes.

The Inspectors tabs:

- Store and Persistent: the values of `store` and `persistent`, as a tree.
- Scene: all layers, back to front, with the tag and kind of each displayable. Select one to change its properties (`x`, `alpha`, `tint_r`, ...). A change applies at once. It is not in the snapshot of the label, so a rollback or a load brings back the old value, and a tween that writes the property overwrites it.
- Tweens: the running tweens.
- Audio: each playing track (file, volume, position) and each silent channel.
- Labels: all labels. The Jump button goes to the first blocking call of that label, with the current `store` (the same as the `vnmaki/goto` request of the debugger). If the label has none, the error goes to the console.

## Screen editor

The window "Screen editor" of the overlay changes the literal values of a screen in your script, and saves them to the file. It works in dev builds, with `vnmaki run`.

1. Press `F12`, and switch on "Pick" in the window "Screen editor". Windows of the overlay can cover the screen; drag them away.
2. Click a node of a screen that shows. The click does not go to the game. The node gets a green outline. The window shows the kind of the node, its rectangle (x, y, width and height in canvas pixels, the result of the layout), and the place of its table in the script (`scripts/ui.lua:12`).
3. Change a field. A number is a decimal number. A string is any text. A boolean is a check box. Examples: `gap`, `align`, `padding`, `bg`, `size`, and the text of a `text` or `button`.
4. Press Save. The editor replaces only the bytes of the values that you changed. All other bytes, and all comments, stay as they were. Hot reload then loads the script again and the screen shows the change. Revert puts the old values back. Deselect clears the selection.

What it can change and what it cannot:

- It changes only a number, a string or a boolean that is written in the table of the node, as in `gap = 12`, `align = "center"`, `disabled = false` or `button { "Start" }`. It does not add or remove a field or a node.
- A field with a computed value (`gap = base + 4`, `size = 10 * scale`, `text { name }`) is shown as read-only with its source text.
- It maps a node to its table by the order of the nodes that the table writes. This works for a screen written as a table, `screen "name" { ... }` or `screen("name", { ... })`. A node is read-only, and the window says why, when:
  - the screen is a function, `screen("name", function(args) ... end)`, or two scripts define it, or the engine makes it (the default screens, the menu, the banner of hot reload);
  - a table above it has an entry that is computed (a call like `rows()`, a name, a `for`), because then the nodes at run time may not be the nodes in the file. The values of that table itself can still change;
  - the file has more or fewer nodes in the table than the screen shows (you edited the file and it did not load yet).
- The game of `vnmaki new` has its quick menu in `scripts/ui.lua` as a table, so you can try the editor on it: pick the row of buttons at the top right, change `gap`, and press Save. The engine's own `quick_menu` is a function and stays read-only; a game that deletes `ui.lua` gets that one.
- `style = "name"` is a string value. The values inside a style (`styles/*.lua`) are not in the screen table, and the editor does not change them.
- If the file changed after you picked the node (for example in your editor), Save does nothing and says so. Pick the node again.
- A string that has the escape `\u{...}` is read-only.

## Hot reload

`vnmaki run` and `vnmaki debug` watch the project folder (not with `--headless`), with all folders below it. Changes that come close together (within 100 ms) are one batch, which is applied between two frames.

| Changed file | What happens |
|--------------|--------------|
| Any `.lua` file | All scripts load again (also the default screens and the styles). The story is replayed to the position where it waits. |
| `shaders/<name>.frag.hlsl` | The game shader is compiled again. A compile error keeps the old shader. |
| `.png`, `.jpg`, `.jpeg`, `.webp` | The texture of that image is loaded again. |
| Other files (audio, fonts, movies, `project.toml`, ...) | Nothing. To see a new audio file or font, restart the label (the Labels tab, or `vnmaki/goto`). |

Rules for scripts:

- The story keeps its label and its step. A line that you changed shows at the same step, and the story goes on from there. The answers of the menus before it are replayed.
- A new Lua VM runs the new scripts. If a script has an error, the old scripts and the story stay as they were.
- `persistent` does not change. No step is added to the rollback history.
- If the label is gone, the story goes to `start` with an empty scene and the store of a new game.
- If the replay cannot reach the old position (the label is shorter, a menu is new, the code fails now), the story stops at the last position that it reached.
- Calls that are in the stack (`call`) stay.

An error goes to the console as an error entry (with `script:line` for Lua, or the shader file and line) and shows a red banner at the top of the window. The banner shows when the overlay is hidden too. It names the first error and counts the others. A file that works again clears its error.

If the operating system cannot watch the folder, hot reload is off and the console has a warning.

## Debugger

`vnmaki debug [<project-dir>] [--headless] [--data-dir <path>]` runs the game in the folder (default: the current folder) and speaks the Debug Adapter Protocol on stdin and stdout. It works only in dev builds on Windows. Nothing else goes to stdout. End of input ends the session as `disconnect` does.

- With a window, the story starts when the client sends `configurationDone`. The window closes when the client disconnects or closes stdin.
- With `--headless`, there is no window. The program plays the story itself (a wait ends at once, a menu takes its first option, a title screen is closed) until it stops at a breakpoint or ends. The tests use this mode.
- `--data-dir` sets the folder for saves and `persistent.vnpersist` (default: the user data folder of the game). Give a test its own folder.

Requests that it handles: `initialize`, `launch`, `attach`, `setBreakpoints`, `setExceptionBreakpoints` (ignored), `configurationDone`, `threads` (one thread), `stackTrace`, `scopes` (`Locals` and `store`), `variables`, `evaluate`, `continue`, `next`, `stepIn`, `stepOut`, `pause`, `disconnect`, `terminate`, and the custom request `vnmaki/goto { label, step = 1 }`. The arguments of `launch` and `attach` are ignored: the project folder comes from the command line.

Rules:

- A breakpoint stops only in script files (`scripts/*.lua`). It does not stop while the story is replayed (a load, a rollback, a reload, a goto). Screens, the REPL and `evaluate` run outside the label, so a breakpoint in a screen function does not stop.
- The window keeps drawing while the story is stopped. The game gets no input.
- `evaluate` answers a local by its name. Another expression runs as in the REPL (`store` and `persistent` yes, locals no).
- `vnmaki/goto` replays the label with no menu answers, to `step`. A label that needs a menu answer before `step` fails. After it, the client gets a `stopped` event with the reason `goto`.

### VS Code

VS Code has no setting that starts an arbitrary program as a debug adapter from `launch.json` alone. A debug type must come from an extension (a `debuggers` entry in its `package.json`). The `package.json` has a `program` field for the adapter, but no field for arguments. So a small extension starts `vnmaki debug <folder>` in its code: the factory of the debug adapter returns a `DebugAdapterExecutable`, which VS Code starts and talks to over stdin and stdout. (The VS Code documentation names these items: the `debuggers` contribution point, `registerDebugAdapterDescriptorFactory` and `DebugAdapterExecutable`. The extension below is not tested in VS Code. The engine side is tested by `cli.debug_*`.)

Make a folder `vnmaki-debug` with two files.

`package.json`:

```json
{
  "name": "vnmaki-debug",
  "version": "0.0.1",
  "publisher": "local",
  "engines": { "vscode": "^1.80.0" },
  "main": "./extension.js",
  "activationEvents": ["onDebug"],
  "contributes": {
    "breakpoints": [{ "language": "lua" }],
    "debuggers": [
      {
        "type": "vnmaki",
        "label": "VNMaki",
        "languages": ["lua"],
        "configurationAttributes": {
          "launch": {
            "properties": {
              "project": { "type": "string", "default": "${workspaceFolder}" },
              "vnmaki": { "type": "string", "default": "vnmaki" }
            }
          }
        },
        "initialConfigurations": [
          { "type": "vnmaki", "request": "launch", "name": "VNMaki: debug", "project": "${workspaceFolder}" }
        ]
      }
    ]
  }
}
```

`extension.js`:

```js
const vscode = require("vscode");

exports.activate = (context) => {
  context.subscriptions.push(
    vscode.debug.registerDebugAdapterDescriptorFactory("vnmaki", {
      createDebugAdapterDescriptor(session) {
        const { project, vnmaki } = session.configuration;
        return new vscode.DebugAdapterExecutable(vnmaki || "vnmaki", ["debug", project]);
      },
    })
  );
};
```

Load it with "Developer: Install Extension from Location..." (or copy the folder to `%USERPROFILE%\.vscode\extensions`). Then the `.vscode/launch.json` of the game:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "vnmaki",
      "request": "launch",
      "name": "VNMaki: debug",
      "project": "${workspaceFolder}",
      "vnmaki": "C:/tools/vnmaki/vnmaki.exe"
    }
  ]
}
```

- `project` must be the folder with `project.toml`. The debugger maps the path of a breakpoint (the path that VS Code sends) to `scripts/...` under this folder, and compares paths in lower case.
- `vnmaki` is the path of the executable. If it is on `PATH`, you can leave it out.
- It must be a dev build.
- To go to a label, call the custom request from an extension command: `vscode.debug.activeDebugSession.customRequest("vnmaki/goto", { label: "warm", step: 1 })`.

## Launcher

`vnmaki launcher` opens a window to work on projects without a command line. `vnmaki` with no arguments does the same in a dev build. In a release build, `vnmaki launcher` prints "this build has no launcher" and exits with 1, and `vnmaki` with no arguments prints the usage. A built game (a `<title>.exe` next to `game.vnpak`) plays the game and never opens the launcher.

- Type the project folder in the text field, or click a folder of the recent list to put it there.
- Buttons, each on the folder of the field:
  - **New** makes a project from the template in that folder (it must not exist, or be empty). Same as `vnmaki new`. A check box for each language add-on of `templates/addons/` (Japanese: `ja`, written in columns) sits under the buttons; each box that is on is a `--lang <code>`. `vnmaki check` finds nothing in the template or with any add-on, so New, Check and Build windows work in this order on a new folder.
  - **Run** plays the game in its own window, as a separate `vnmaki run <folder>` process. The launcher stays open, and a crash of the game does not close it. The messages of the game go to the console that started the launcher, not to the log.
  - **Check**, **Extract strings** and **Build windows** do what `vnmaki check`, `vnmaki extract-strings` and `vnmaki build windows` do, and the log shows their output. The build goes to `<folder>/.vnmaki/windows` and uses the release `vnmaki.exe` of `build/release/cli` as the runner (see `docs/CONVENTIONS.md`).
  - **Open folder** shows the folder in Windows Explorer.
- The log shows the command line (`> vnmaki check "..."`) and then its output. **Clear log** empties it.
- A folder goes to the recent list (the last 10, newest first) when its action succeeds. The list is the file `recent.txt` in the folder `%APPDATA%\VNMaki\launcher`. The launcher cannot keep a folder name with characters outside the code page of the system.
- `vnmaki launcher --frames N` ends after N frames, and `--data-dir <path>` puts `recent.txt` in `<path>`. The tests use them.
- Only Windows has Run and Open folder.

## `vnmaki check`

`vnmaki check <dir>` lints a project without running it. It prints CSV with the header `file,line,severity,code,message` and one row for each finding, sorted by file and line (the line is empty when it is not known). The exit code is 0 when there is no error (warnings are fine), 1 when there is an error or `<dir>` is not a folder, and 2 for a usage error. The template of `vnmaki new` gives no finding.

Only string literals are checked.

| Code | Severity | Finding |
|------|----------|---------|
| `lua-syntax` | error | A script does not load. The line comes from the Lua message. |
| `unknown-label` | error | A `jump`, a `call` or a menu branch to a literal label that no script defines. |
| `bad-text-tag` | error | The literal text of `say` or `narrate` has a text tag that does not parse (the message has the byte offset). |
| `missing-image` | error | A literal name in `scene` or `show` has no image file. |
| `missing-audio` | error | A literal name in `music`, `sound` or `voice` has no audio file (a voice is under `audio/voice/`). |
| `missing-movie` | error | The path of a `movie` call has no file. |
| `po-syntax` | error | A `translations/*.po` file does not parse. |
| `bad-translation-tag` | error | A translation in use (not fuzzy, not obsolete, not empty, not a `screen/` key) has a text tag that does not parse. |
| `unreachable-label` | warning | No path of `jump`, `call` and menu edges from `start` reaches the label. |
| `dead-end` | warning | A label has no `jump` and no `ret`, is not marked `{ ending = true }`, and no `call` names it. |
| `dynamic-edge` | warning | A `jump` or `call` with a computed target. |
| `project-config` | warning | An error in `project.toml` (the line is empty for a missing file or key). |
| `non-literal-string` | warning | The text of `say` or `narrate`, a menu option, or the first value of a screen `text` or `button` is not one string literal, so `vnmaki extract-strings` cannot extract it. |

`show("x")` is not checked when `x` is the first argument of a `layered(...)` or `animation(...)` call in any script, or when its table has the key `image` or `movie`. Mark a label that ends the story with `{ ending = true }` as the third argument of `label`.

## `vnmaki graph`

`vnmaki graph <dir> [--format dot|csv]` prints the story graph of the scripts, as Graphviz DOT (the default) or CSV. It does not run the story, so it shows no traces; the overlay shows the traces. The CSV header is `type,from,to,script,line,flags`, with a row for each label and then for each edge. Flags join with `;`: `ending`, `unreachable` and `dead_end` for a label, `dynamic` for an edge. The type of an edge is `jump`, `call` or `menu`. Exit code: 0, 1 if `<dir>` is not a folder, 2 for a usage error.

## `vnmaki extract-strings`

`vnmaki extract-strings <dir>` writes `translations/template.pot` and updates `translations/<lang>.po` for each language of `project.toml` after the first one (the source language). It prints one line for each file: `path: N strings`, and for a `.po` file also `, F fuzzy, O obsolete`.

- Only non-empty string literals are extracted: `say(who, text)`, `narrate(text)`, the options of `menu { ... }`, and the first value of a screen `text { ... }` or `button { ... }`. `vnmaki check` warns (`non-literal-string`) about the others.
- The key of an entry is `<scope>/<hash>`. The scope is the label name or `screen/<name>`. The hash is FNV-1a (32 bit, 8 hex digits) over the speaker, a NUL byte and the text. If you rename a character variable, the keys change, and the old translations move to the new keys as fuzzy.
- An old translation whose key is gone moves to a new key in the same place of the same scope (fuzzy), or becomes obsolete (it stays at the end of the file, and comes back when the key is extracted again). A second run on the same scripts changes nothing.
- The command works out all files first. A `.po` file that cannot be read gives exit 1 and changes no file.

## Profile CSV

`vnmaki run <dir> --frames N --profile out.csv` plays N frames in a window (not with `--headless`) and writes two files when the window closes. It works in a release build too, and that is the build whose numbers matter.

`out.csv` has a row for each frame:

| Column | Meaning |
|--------|---------|
| `frame` | The number of the frame. |
| `events_us` | Time to handle the events. |
| `update_us` | Time of the fixed-step updates of the frame. |
| `prepare_us` | Time to upload what the frame needs before the first pass. |
| `draw_us` | The rest of the draw hook (the passes). |
| `total_us` | The sum of the four times above. It is CPU time and has no wait for the display. |
| `updates` | How many fixed updates ran in the frame. |
| `passes`, `quads`, `draw_calls` | The GPU counters of the frame. |
| `texture_bytes`, `target_bytes` | Bytes of textures and of render targets. |
| `textures`, `targets` | How many there are. |

Times are whole microseconds. `out_loads.csv` (the same name with `_loads` before the extension) has the header `path,time_us` and a row for each asset load (read and decode) in the order in which they ended, for example `fonts/default.ttf,<time>`.

The overlay panel Profiler shows the same numbers for the last 300 frames.
