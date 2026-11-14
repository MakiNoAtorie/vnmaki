# Text tags

Text for `say` and `narrate` can contain tags. A tag changes how a part of the text looks or how the typewriter reveal shows it.

```lua
say("Kai", "I {color=#f00}really{/color} mean it.{pause=0.5} Truly.")
```

## Syntax

- A tag is `{name}` or `{name=value}`. The tag `{/name}` closes it.
- Close the tags in the opposite order to the order in which you opened them: `{b}{i}text{/i}{/b}`.
- Close each tag before the text ends. `pause` is the only tag that you do not close.
- Write `{{` for the character `{`. The character `}` needs no special syntax outside a tag.
- A value cannot contain `{` or `}`.
- A number has digits and, optionally, one decimal point: `2`, `0.5`. It has no sign and no exponent.

## Tags

| Tag | Value | Effect |
| --- | --- | --- |
| `{color=#rrggbb}` | `#rgb`, `#rgba`, `#rrggbb` or `#rrggbbaa` | Sets the text colour. |
| `{size=30}` | More than 0, not more than 1024 | Sets the text size, in virtual pixels for each em. |
| `{font=name}` | The name of a font family | Uses that font family. An unknown name uses the default family. |
| `{b}` | None | Uses the bold font of the family. |
| `{i}` | None | Uses the italic font of the family. |
| `{speed=40}` | More than 0, not more than 100000 | Sets the reveal speed, in characters for each second. |
| `{pause=0.5}` | 0 to 100000 | Stops the reveal for that number of seconds at this position. |
| `{ruby=text}` | The ruby text | Shows the ruby text (furigana) above the text in the tag. |
| `{link=id}` | An identifier | Makes the text in the tag a link. |

`ruby` tags cannot contain other `ruby` tags. `link` tags cannot contain other `link` tags.

## Errors

A tag that the engine cannot use stops the story with a script error. The message gives the script line, the byte position of the tag in the text, and the cause:

```
scripts/main.lua:4: bad text tag at byte 2: unknown tag 'colr'
```

## Links

Give `on_link` a function when the scripts load. The engine calls the function with the identifier when the player selects a link.

```lua
on_link(function(id)
  print("link " .. id)
end)
```

The function runs outside the labels:

- It cannot call `say`, `narrate`, `menu`, `jump` or `call`.
- It must not change `store`. A replay does not call the function again.

## Text direction, line breaks and fonts

- Right-to-left text (for example Hebrew and Arabic) and mixed text show in the correct visual order. The reveal and the caret follow the order in which the text is read.
- In Japanese text, a line does not start with a closing mark, a small kana or a long vowel mark. A line does not end with an opening bracket.
- A line does not break in the text of a `ruby` tag.
- If the font does not have a character, the engine uses the first font in the fallback chain of the language that has it. Set the chains in `project.toml`:

```toml
languages = ["en", "ja"]

[fonts.fallback]
en = ["fonts/latin.ttf"]
ja = ["fonts/japanese.otf", "fonts/latin.ttf"]
```

## Effect text

Normal text uses glyph bitmaps at the screen resolution. Text with an outline or a shadow uses a signed-distance-field glyph atlas (`GlyphRaster::kDistance`) and the shader `sdf_text_shader()`. See `engine/text/text.hpp`.
