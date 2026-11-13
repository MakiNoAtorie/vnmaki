#!/usr/bin/env python3
"""Generate the project of the screen editor tests (tests/unit/screen_editor_test.cpp).

Usage: screen_editor_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

`project/` is a game with the screens `menu`, `cells`, `dyn` and `fn` in `scripts/ui.lua`, which
has CRLF line breaks, comments in odd places, single and double quotes, a negative number, a
string with escapes and non-ASCII text, and some computed values. `expected/scripts/ui.lua` is
`ui.lua` after the edits that the test makes; the test compares the two files byte for byte.

The edits are the SLOTS below: `@name@` in the template is the old text in `project/` and the new
text in `expected/`. Nothing here reads the engine's scan: the expected bytes are the template
text with the slots filled in, so any other byte (a comment, a space, a line break) must be the
same in the two files.
"""

import shutil
import sys
from pathlib import Path

# name: (the literal as written now, the literal after the edit that tests/unit/
# screen_editor_test.cpp makes). The new text is what the engine writes: double quotes unless the
# old literal had single quotes, escapes for the quote, backslash and line breaks, other bytes as
# they are.
SLOTS = {
    "gap": ("12", "20"),
    "align": ('"start"', '"center"'),
    "hello": ('"Hello \\"there\\""', '"Line \\"two\\" Ünï ✓"'),
    "size": ("30", "31.5"),
    "start": ("'Start'", "'Go'"),
    "disabled": ("false", "true"),
    "x": ("-6", "4"),
    "sound": ('"Sound"', '"Line1\\nLine2"'),
    "level": ("5", "7.25"),
}

UI_TEMPLATE = """\
-- Screens of the screen editor fixture. Every comment must stay.
local pad = 7  -- a computed value
local function rows() return hbox { text { "row" } } end

screen "menu" {  -- one root node
  vbox { gap=@gap@ , align = @align@, -- no space around the first =
    text { @hello@ , size = @size@ },
    --[[ a block comment ]] button { @start@, returns = "start", disabled = @disabled@ },
    hbox { x = @x@, y = 300, padding = pad, gap = 1,
      text { @sound@, size = 10 + 20 },
      bar { min = 0, max = 10, value = @level@, width = 100, height = 10 }, -- last in hbox
    },
    text "short form",  -- the string form of a node
  },
}

screen("cells", {
  grid { columns = 2, gap = 3,
    text { "a" }, text { "b" },
    text { "c" },
  },
})

screen "dyn" {
  vbox { gap = 4,
    rows(),
    text { "tail" },
  },
}

screen("fn", function(args)
  return vbox { gap = 9, text { "in a function" } }
end)
"""

MAIN = """\
label("start", function()
  show_screen("menu")
  show_screen("cells")
  show_screen("dyn")
  show_screen("fn")
  narrate("ready")
end, { ending = true })
"""


def fill(which):
    text = UI_TEMPLATE
    for name, pair in SLOTS.items():
        text = text.replace("@" + name + "@", pair[which])
    assert "@" not in text
    return text.replace("\n", "\r\n").encode("utf-8")


def main():
    out = Path(sys.argv[1])
    if out.exists():
        shutil.rmtree(out)
    for folder, which in (("project", 0), ("expected", 1)):
        scripts = out / folder / "scripts"
        scripts.mkdir(parents=True)
        (scripts / "ui.lua").write_bytes(fill(which))
    (out / "project" / "scripts" / "main.lua").write_bytes(MAIN.encode("utf-8"))
    (out / "project" / "project.toml").write_bytes(b'title = "Screen editor fixture"\n')


if __name__ == "__main__":
    main()
