# Plugins

A plugin adds to the engine without a change to the engine. There are two kinds (SPEC section 20):

- A **Lua plugin** is a folder of the game. It adds API functions, screens, transitions and effects. It needs no build.
- A **C++ plugin** is a static library. It adds Lua functions, displayable types and file formats. It is linked into the runner at build time.

Dynamic plugin DLLs and plugins that add shaders or platform services are not in the engine yet.

## Lua plugins

A Lua plugin is the folder `plugins/<name>/` of the project. `<name>` has the characters a-z, 0-9, `_` and `-`.

```
my_game/
  project.toml           plugins = ["flash"]
  plugins/flash/
    plugin.toml
    flash.lua
  scripts/
    main.lua
```

`project.toml` lists the active plugins. A folder in `plugins/` that the list does not name does nothing.

```toml
title = "My game"
plugins = ["flash"]       # before any [table]
```

`plugin.toml`:

```toml
name = "flash"                # optional. If it is there, it must be the folder name.
version = "1.0.0"             # optional. For people.
engine = ">=0.1.0, <0.2.0"    # required. The engine versions that the plugin works with.
```

### The engine version range

The range is a list of comparators. Separate them with commas or spaces. A comparator is `>=`, `>`, `<=`, `<` or `=` (no sign means `=`), then `major`, `major.minor` or `major.minor.patch` (a part that is missing is 0). The engine must meet every comparator.

If the engine is not in the range, the game does not start. The message names the plugin, the range and the engine:

```
vnmaki: plugin 'flash' needs vnmaki >=9.0.0, but this is vnmaki 0.1.0 (plugins/flash/plugin.toml)
```

The same check stops the start when the plugin is in the list and its folder or `plugin.toml` is missing, or when `plugin.toml` is not valid. No script of a plugin runs before all the manifests are checked.

### Load order

1. The engine's default screens.
2. The plugins, in the order of the `plugins` list. For one plugin, every `.lua` file in its folder (also in subfolders) in byte order of the path.
3. The `.lua` files of `scripts/`.

So the scripts of the game can use what a plugin defines, and can replace a screen that a plugin defines. A plugin runs in the same sandbox as the scripts. It can define labels, but it should not: a plugin adds tools, and the game has the story.

A plugin file is a Vfs file like any other. A game that `vnmaki build windows` packs has its plugins in `game.vnpak`, and they load from there.

### What a Lua plugin can add

- **API functions.** Define a global function in a plugin file. Unlike in a game script, a new global in a plugin gives no dev warning. The function can call `say`, `show`, `tween`, `wait` and the rest, so it can block like a label.

  ```lua
  function shout(text)
    return string.upper(text) .. "!"
  end
  ```

- **Screens.** Use `screen "name" { ... }` as in a script.
- **Transitions.** `define_transition(name, base, options)` makes `name` a transition that runs the transition `base` (a built-in one, or a game shader) with these options as its defaults. A call can still give other options. `base` cannot be another name made with `define_transition`.

  ```lua
  define_transition("flash", "fade", { color = "#ffffff", duration = 0.25 })
  -- in a script:  transition("flash")   or   show("eileen", { with = "flash" })
  ```

- **Effects.** A function that mixes calls: `shake`, `camera`, `tween`, `shader`.

### Limits

- The game shaders that a plugin needs are files of the game (`shaders/<name>.frag.hlsl`). The engine does not look for shaders in `plugins/<name>/`.
- Hot reload (dev tools) watches `scripts/`, not `plugins/`. Start the game again after a change to a plugin.
- `vnmaki check` does not read `plugins/`. It also does not flag a call to a function that no script defines, so the functions of plugins are not flagged. A `show` that has the option `displayable` is not checked for an image.
- A plugin cannot be turned on or off while the game runs.

### The plugin of the template

`vnmaki new` makes a game with a Lua plugin, `plugins/flash/`. `project.toml` lists it (`plugins = ["flash"]`), `plugin.toml` gives the engine range `>=0.1.0`, and `flash.lua` is one line: `define_transition("flash", "fade", { color = "#ffffff", duration = 1 })`. The label `content` (`scripts/content.lua`) uses it with `show("eileen", { at = "center", with = "flash" })`. Packs hold it like any file of the game, so the built game has it too.

## C++ plugins

A C++ plugin is a folder `plugins/<name>/` of the engine source tree with a `CMakeLists.txt`:

```cmake
vnmaki_add_plugin(my_plugin SOURCES my_plugin.cpp my_plugin.hpp)
```

`vnmaki_add_plugin` (in `cmake/VnmakiPlugins.cmake`) makes the static library `vnmaki_plugin_my_plugin`. It links `vnmaki::engine`, so the plugin can include any engine header. `<name>` is lower case letters, digits and `_`.

The sources define one function in the namespace `vnmaki`. The engine calls it once, at start:

```cpp
#include "engine/plugin/plugin.hpp"

namespace vnmaki {

void register_plugin_my_plugin(PluginContext& context) {
  context.add_function("my_double", [](std::span<const PluginValue> args) -> PluginValue {
    // ...
  });
}

}  // namespace vnmaki
```

To link plugins into the runner, configure with their names:

```
cmake --preset debug -B build/debug-plugins "-DVNMAKI_PLUGINS=example;my_plugin"
```

CMake makes a source file that registers the plugins in `PluginContext::linked()` before `main`. A name that no `vnmaki_add_plugin` made is a configure error. A plugin that throws `PluginError` while it registers stops the program with its name. Every runtime and scene uses `PluginContext::linked()` unless a test gives it another one (`ScriptRuntimeOptions::plugins`, `Scene::set_plugins`).

The public header `engine/plugin/plugin.hpp` has no Lua and no sol2. The engine turns Lua values into `PluginValue` and back.

### Lua functions

```cpp
using PluginFunction = std::function<PluginValue(std::span<const PluginValue>)>;
context.add_function("name", function);
```

- `PluginValue` is nil (`std::monostate`), a boolean, an integer (`std::int64_t`), a float (`double`) or a string. A table or a function cannot cross.
- The name is a Lua name (not a keyword). It becomes a global function when the scripts load. It must not replace a global of the engine (`say`, `store`, ...). Two plugins cannot use the same name.
- Throw `PluginError` for a bad argument. It is a script error at the line of the call, with the function name in front: `scripts/main.lua:5: example_add: needs two numbers`.
- A replay (a load, a rollback) calls the function again. It must give the same result for the same arguments, and it must not change what the story depends on.

### Displayable types

```cpp
class BadgeDisplayable final : public PluginDisplayable { ... };
context.add_displayable("example_badge", [](const PluginParams& params) { ... });
```

A displayable type is a `PluginDisplayable` (a `Displayable`, see SPEC section 6) and a factory. A script shows one with:

```lua
show("badge", { displayable = "example_badge", params = { size = 100, color = "#ff8800" } })
```

- The tag, `at`, `x`, `y`, `layer`, `with`, shaders and tweens work as for an image. `image` is the tag.
- `params` is a table of booleans, numbers and strings. The factory gets them as `PluginParams`, sorted by name. `number_param` and `string_param` read one with a default. The factory throws `PluginError` for a bad value, and the script gets the message at the line of the `show`.
- `PluginDisplayable::white_texture()` is a 1 by 1 white texture, to draw solid colours (the `tint` of a quad is the colour). It is the zero id until the first `Scene::prepare()`.
- A snapshot (a save, a rollback) keeps the type, the params and the properties (position, alpha, tint, ...) and the position in the layer. After a load the factory makes the object again, so what the object keeps in members is lost. A game that loads a snapshot with a type that no plugin adds fails the load.
- The factory and the displayable run on the main thread.

### Image formats

```cpp
context.add_image_loader(".ppm", [](std::span<const std::byte> bytes) -> PluginImage { ... });
```

`images/<name>.ppm` is then an image name for `show` and `scene`, found after `.png`, `.jpg` and `.webp`. The loader returns 8-bit RGBA with straight (not premultiplied) alpha and throws `PluginError` for bad data. It runs on a job thread. The extension is lower case with the dot, and cannot be a built-in one. The table of formats is process-wide (`register_image_format` in `engine/render/image.hpp`).

## The example plugin

`plugins/example/` is a C++ plugin and the model for your own. It adds `example_add(a, b)`, `example_greet(name)`, the displayable `example_badge` (a square: params `size` and `color`) and the image format `.ppm`.

The tests:

- `tests/unit/plugin_test.cpp`: version ranges, manifests, Lua plugins (load order, functions, transitions, a version mismatch, packs), the context, functions, displayables and snapshots, image formats.
- `tests/render/plugin_render_test.cpp`: a GPU golden of the badge and a `.ppm` picture.
- `tests/cli/CMakeLists.txt`: a Lua plugin transition and function (`cli.plugin_lua_adds_a_transition_and_a_function`), the version mismatch (`cli.plugin_version_mismatch_stops_the_start`), a Lua plugin from packs, and, in a build with `-DVNMAKI_PLUGINS=example`, the C++ plugin in a run.
- `tools/generators/plugin_fixtures.py` writes the projects and the golden.
- `tests/unit/content_demo_test.cpp` and `tests/render/content_render_test.cpp`: the Lua plugin of the template (`flash`) in the content demo, played from packs and drawn on a GPU (the transition at progress 0.25).
