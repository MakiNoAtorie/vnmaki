# Script reference: presentation

This is the list of the Lua calls for pictures, motion, shaders, movies and lip sync. The calls for text, menus, audio, saves and screens are in `docs/SPEC.md`. A complete example is the label `presentation` in `templates/default/scripts/presentation.lua`.

Rules for all calls:

- Times are in seconds.
- `x` and `y` are fractions of the virtual resolution: width for `x`, height for `y` (0.5 is the center). `rotation` is in degrees, clockwise. `scale`, `alpha`, `anchor` and `crop` are fractions.
- A call with a wrong argument, an unknown name or a missing file stops the story with an error at the line of the call.
- A call that must be inside a label raises an error if it runs outside one.
- A replay (a load, a rollback) runs every call again. Tweens, transitions, the camera and the shake then end at once and show no motion. A replay counts the same steps as the first play.

## Layers and tags

- `show` puts a picture on a layer and gives it a tag. A tag is unique on all layers. `hide(tag)` removes it.
- Layers from the back: `background`, `master`, `characters`, `3d`, `overlay`, `ui`, `transient`. `scene(image)` clears `background`, `master` and `characters` and shows the image on `background`. The other layers stay. The default layer of `show` is `characters`.
- `show(tag, options)`: `image` (an image name; the tag if you leave it out), `at` (`"left"`, `"center"` or `"right"`: 0.25, 0.5 or 0.75 of the width, at the bottom), `x`, `y`, `layer`, `movie`, `loop`, `shader`, `uniforms`, `with`, and the groups of a layered sprite. A `show` of a tag that is shown keeps the position and changes what the options name.
- Image name `N` is the file `images/N.png`, then `.jpg`, then `.webp`.
- Special tags for `tween` and `wait`: `@camera`, `@layer.<name>` (the uniforms of a layer) and `@transition`. A `show` tag cannot be one of them.

## tween, sequence, parallel, wait

`tween(tag, options)` changes the properties of the tag over time. It starts at once and does not block.

| Option | Meaning |
|--------|---------|
| `x`, `y` | Position, as for `show`. |
| `anchor = { x, y }` | Anchor of the picture. |
| `scale = n` or `{ x, y }` | Scale. |
| `rotation`, `alpha` | Degrees; 0 to 1. |
| `tint = "#rrggbb"` | Color that multiplies the picture. |
| `crop = { x, y, width, height }` | The part of the picture that shows. |
| `uniforms = { name = n }` | Values of the shader of the tag. |
| `duration` | 0.5 if you leave it out. |
| `delay` | Seconds before the start. The start values are read then. |
| `ease` | `linear` (default), or `in_`, `out_` or `in_out_` plus `sine`, `quad`, `cubic`, `back` or `bounce`. |
| `loop`, `yoyo` | `loop = true` repeats for ever. `yoyo = true` goes to the target and back. Alone, it does this one time. |

- A property that the call does not name stays as it is. A tween of a tag that is not shown does nothing.
- Tweens of one property in a `sequence` continue from the last end value.
- `sequence { item, ... }` runs the items one after the other. `parallel { item, ... }` runs them together. Both start at once. An item is `{ tag, <options of tween> }`, or a group: `{ sequence = { ... } }` or `{ parallel = { ... } }`. The table of a group can have `delay` and `loop`. A group nests up to 16 levels.
- A loop in a `sequence` must be its last item. A nested group cannot loop. The call returns an error otherwise.
- `wait(tag)` blocks until the tweens of the tag have ended. `wait_all()` blocks for every tween. A wait is a step even when nothing is tweening, and rollback goes back over it. A loop, a looping animation and a looping movie never block a wait.
- A click or skip mode ends the wait: the tweens go to their end values.

```lua
tween("eileen", { x = 0.25, duration = 1, ease = "out_quad" })
sequence {
  { "eileen", y = 0.9, duration = 0.2 },
  { "eileen", y = 1, duration = 0.2 },
  loop = true,
}
parallel { { "eileen", alpha = 0.5 }, { "hud", rotation = 90 } }
wait("eileen")
```

## Transitions

`transition(name, options)` mixes the frame from before the changes of this step into the frame after them. It blocks until it ends, like `wait`, with the tag `@transition`.

- `scene`, `show` and `hide` take `with = "dissolve"` or `with = { "wipe_left", duration = 1 }`. All the changes since the last blocking call belong to the transition that the step ends with, so give `with` to the last change of the step (`scene("bg/a")` then `show("eileen", { with = "dissolve" })`). A `with` with a bad name changes nothing.
- Names: `dissolve`, `fade`, `slide_left`, `slide_right`, `slide_up`, `slide_down`, `wipe_left`, `wipe_right`, `wipe_up`, `wipe_down`, `mask`. For slides, the direction is the way the content moves. For wipes, it is the way the edge moves.
- Options: `duration` (0.5 if you leave it out; 0 makes no transition), `color` (`fade`: `"#rrggbb"`, black by default), `softness` (`wipe_*` and `mask`: 0 to 1), `mask` (`mask`: an image name; the new picture shows first where the mask is dark), `uniforms` (a custom transition). An option that the transition does not use is an error.
- Any other name is a game shader (see Shaders) that includes `transition.hlsli` and declares the uniform `progress`. The engine sets `progress` (0 to 1). The other uniforms come from `uniforms`.
- The layers from `background` to `overlay` are mixed. `ui` and `transient` draw over the result.

## Layered sprites

```lua
layered("eileen", {
  order = { "base", "outfit", "face" },
  base = "eileen/base",
  outfit = { school = "eileen/outfit_school" },
  face = { smile = "eileen/face_smile", sad = "eileen/face_sad" },
  default = { outfit = "school", face = "smile" },
  crossfade = 0.25,
})
show("eileen", { face = "sad" })
```

- `order` lists the groups from the back. A group is one image (a string), or a table `value = "image"`. A value can also be the name of an animation. Layers have their own size and all start at the top-left corner of the sprite. The first group is the base: its image gives the size that `anchor`, `scale` and `crop` use.
- `default` has a value for each group that shows at first. A group without a default shows nothing.
- `show` with a defined name shows the sprite. It changes only the groups that it names. An unknown group or value is an error. Do not name a group like an option of `show` (`with`, `image`, `at`, `x`, `y`, `layer`, `shader`, `uniforms`, `movie`, `loop`).
- `crossfade` (seconds; none by default) makes a new image fade in over the old one. A replay and a load cut at once.
- `layered` can be called again with the same name to replace the definition. Define it before the first `show`.

## Frame animations

```lua
animation("blink", { frames = { "eyes_open", "eyes_half", "eyes_closed" }, fps = 8, loop = true })
animation("spark", { sheet = "spark_sheet", columns = 4, rows = 2, count = 7, fps = 12, loop = false })
show("light", { image = "spark" })
```

- `frames` are image names. A `sheet` is one image with `columns` times `rows` cells, read row by row; `count` is the number of cells that are frames (all by default). Use `frames` or `sheet`, not both. `fps` is required (above 0, up to 1000). `loop` is true by default. There are at most 4096 frames.
- `show` takes an animation name as it takes an image name. A value of a layered group can be an animation: `lids = { open = "eyes_open", blink = "blink" }`. Define the animation before the layered sprite.
- The animation starts at frame 1 when it is shown, or when the group takes the value. After a load or rollback, it starts again at frame 1.
- With `loop = false`, the last frame stays, and `wait(tag)` waits for it.

## 3D scenes

A 3D scene draws glTF 2.0 models (`.gltf` or `.glb`, with textures) with a camera and lights into a picture of the screen size. The picture is on a layer (`3d` by default), so it mixes with the 2D layers, and it follows the 2D camera and tweens like an image.

```lua
scene3d("stage")
camera("stage", { position = { 0, 1.5, 6 }, look_at = { 0, 1, 0 }, fov = 45 })
light("stage", "sun", { kind = "directional", direction = { -0.3, -1, -0.5 }, intensity = 0.9 })
light("stage", "fill", { kind = "ambient", intensity = 0.2 })
model("hero", "models/hero.glb", { scene = "stage", shader = "toon", rotation = { 0, 30, 0 } })
tween("@3d.stage.hero", { ry = 210, duration = 2, ease = "in_out_sine" })
wait("@3d.stage.hero")
hide("stage")
```

- `scene3d(name, { layer = "3d" })` makes the scene, or moves it to another layer. Its tag is `name`, so `hide(name)` removes it with its models, and `tween(name, { alpha = 0.5 })` fades it.
- `model(tag, path, { scene, position = { x, y, z }, rotation = { x, y, z }, scale, shader, uniforms })` shows a model. `path` has its extension. The rotation is in degrees, and `scale` is one number. Calling again with the same tag changes only the options you name.
- `shader` is `"unlit"` (the colour of the material, no light; the default), `"toon"` (three flat bands of light), `"pbr"` (the metallic and roughness of the material), or the name of a shader declared with `shader(name, { uniforms })`. A shader of yours must include `mesh.hlsli` (see `docs/CONVENTIONS.md`, Render). `uniforms` sets its values.
- `light(scene, name, { kind, direction, position, color = "#rrggbb", intensity })`: `kind` is `"directional"` (the default; `direction` is the way the light travels), `"point"` (`position`; it weakens with the square of the distance) or `"ambient"` (the same everywhere). A scene has at most 4 lights besides ambient ones. A lit shader shows black without a light.
- `camera(scene, { position, look_at, fov })` sets the camera: the default is at `{ 0, 0, 5 }` looking at the origin, with a field of view (up and down) of 60 degrees. `camera { x = 0.1 }` with a table and no scene name is the 2D camera.
- `hide3d(scene, name)` removes a model or a light.
- A point is `{ x, y, z }` (y is up, and the camera looks down -z by default). A name has no `.` and does not start with `@`.
- Tweens and `wait` use the tag `@3d.<scene>.<name>`, where `<name>` is `camera`, a model or a light. A model has `x y z rx ry rz scale` and the uniforms of its shader, a light has `x y z r g b intensity`, and the camera has `x y z lx ly lz fov` (look-at point). They are options of `tween`, with `duration`, `delay`, `ease`, `loop` and `yoyo`, and `parallel` and `sequence` work with them. Other options (`alpha`, `crop`, ...) are an error.
- 3D state is in saves and rollback. After a load or a rollback, loops start again, and tweens that end are at their end values.

## Camera, shake and parallax

The camera moves the layers from `background` to `overlay`. `ui` and `transient` stay.

- `camera { x, y, zoom, rotation, duration, delay, ease }` is a tween of `@camera`. Without `duration` and `delay` it is instant. `x` and `y` are the pan, as fractions of the width and height: a positive pan moves the view right and down, so the content moves left and up. `zoom` is above 0 (1 is none; `scale` is the same). Zoom and rotation are about the center of the screen. `loop` and `yoyo` work. `tween("@camera", ...)` allows only these four properties.
- `shake { strength, duration }`: `strength` (required, 0 to 1) is the largest offset as a fraction of the screen height. `duration` is 0.5 if you leave it out. It dies away smoothly, and it is a fixed function of the time (no random numbers). A new shake replaces the old one.
- `camera` and `shake` do not block. `wait("@camera")` waits for a move and a shake.
- `layer(name, { parallax = f })` sets the pan of a layer to `f` times the camera's pan and shake. The default is 1; the range is -100 to 100. A layer that the camera does not move is an error. With `parallax = 0`, the layer does not move.
- The camera, the parallax and the shader of a layer stay when `scene` is called. Reset them in the script.
- Reduced motion: if `persistent.prefs.reduce_motion` is set, camera moves are instant (the `wait` step stays) and `shake` does nothing.

## Shaders

- `shader(name, { "amount", ... })` declares the file `shaders/<name>.frag.hlsl` and its uniforms. The uniforms are floats, up to 16, in the order of the list. The shader compiles when the scripts load. A script error shows the file, the line and the message. Declaring a name again with the same uniforms does nothing.
- The file includes `vnmaki.hlsli` and returns a color with premultiplied alpha. It can read the drawn picture with `sample_sprite(input)`, and it can have one constant buffer at `register(b0, space3)` for the uniforms. See `templates/default/shaders/sepia.frag.hlsl`.
- `show(tag, { shader = "sepia", uniforms = { amount = 0.5 } })` puts a shader on a picture. `layer(name, { shader = "sepia", uniforms = { amount = 0.5 } })` puts it on a whole layer: the layer draws into its own picture, and the shader works on the result. `shader = ""` removes it. A `show` without these options keeps them. Uniforms that you do not name keep their values, and a new shader starts with zeros.
- `tween(tag, { uniforms = { amount = 1 } })` changes the values. The tag of a layer is `@layer.<name>`: `tween("@layer.background", { uniforms = { amount = 1 }, duration = 1 })`.
- Every kind of picture (image, layered sprite, animation, movie) takes a shader the same way.

## Movies

- `movie(path, { skippable = true })` plays the file over all layers and blocks until it has ended. The path has the extension: `"movies/clip.webm"`. A movie is AV1 video, with optional Opus audio, in WebM. `skippable` is false by default; a click then does not end it, but skip mode does.
- A replay and the headless runner do not play it. Rollback goes back over it, as over a wait.
- `show(tag, { movie = "movies/loop.webm", loop = true })` shows a movie as a picture in a layer. A movie that does not loop is what `wait(tag)` waits for. After a load or rollback it plays from the start.

## Lip sync

The mouth of a layered sprite follows the level of a voice.

```lua
layered("eileen", {
  order = { "base", "mouth" },
  base = "eileen/base",
  mouth = { closed = "eileen/mouth_closed", half = "eileen/mouth_half", open = "eileen/mouth_open" },
  default = { mouth = "closed" },
  lip_sync = { group = "mouth", frames = { "closed", "half", "open" }, thresholds = { 0.05, 0.2 },
               voice_tag = "eileen" },
})
local eileen = character { name = "Eileen", voice_tag = "eileen" }

voice("eileen/hello")
say(eileen, "Hello.")
```

- `group` is a group in `order`. `frames` are two or more values of it, the closed mouth first. `thresholds` are one fewer than the frames, rising, above 0 and up to 1. The mouth shows the frame whose thresholds the level has reached. The level is the RMS of the voice (0 to 1), before any volume.
- `voice_tag` is the voice to follow (the name of the definition by default). `voice(name)` plays `audio/voice/<name>`. A `say` with a character that has `voice_tag`, right after `voice`, gives the voice its tag.
- The mouth shows the first frame when no voice with the tag plays. It does not change `show` values, saves or rollback. A replay plays no voice, so the mouth stays closed.
- `vnmaki run` stops the voice while skip mode is on.

## Languages

- `languages = ["en", "ja"]` in `project.toml` lists the languages. The first is the one the scripts are written in.
- `vnmaki extract-strings <folder>` writes `translations/template.pot` and `translations/<code>.po`. Translate the `msgstr` lines. A fuzzy or empty entry shows the source text.
- `vnmaki run <folder> --lang ja` or `prefs.language = "ja"` plays in that language. `languages()` lists the codes. A button that returns `"@language:ja"` changes the language at once; the default `preferences` screen has these buttons when there is more than one language.
- Images and voices: `images/ja/<name>` and `audio/voice/ja/<name>` are used first in that language.
- `[fonts.fallback]` gives the font chain of a language. `[text_direction]` with `ar = "rtl"` reverses the order of an `hbox`. `[styles]` with `ja = "styles/ja_look.lua"` (or `styles/<code>.lua`) is a file of `style(...)` calls that runs over the game styles.
- A dev build has a "Translations" window in the overlay (F12): it lists the entries of the current label and saves an edit to the `.po` file.
## Captions and reduced motion

```lua
sound("sfx/door", { caption = "a door slams" })
music("bgm/theme", { fade = 1.5, caption = "soft music" })
ambient("bgm/night", { caption = "wind" })   -- music on the channel "ambient"
```

- `caption` (a string) is what the captions screen says when the player turns on Captions in the preferences. A sound's caption shows as long as the sound; a music's caption shows for 5 seconds, and only when the music changes. A replay shows none. `ambient` takes the options of `music`.
- The preference Reduce motion stops shakes, makes `camera` moves instant and turns every transition (`transition(...)`, `with = ...`) into a dissolve of at most 0.2 seconds.
