#!/usr/bin/env python3
"""Generate the game projects the `vnmaki run` CLI tests run.

Usage: runtime_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Each project is a folder with a project.toml and Lua scripts under scripts/;
the projects in IMAGES also get image files under images/.
For projects that run to the end, OUT_DIR/<name>.transcript holds the exact
stdout `vnmaki run <project> --headless` must print, written by hand from
the scripts below. The numbers the "dice" project rolls come from the
splitmix64 written out here, not from the engine.
"""

import shutil
import sys
from pathlib import Path

import text_fixtures
from render_fixtures import png_rgba

PROJECTS = {
    # Shows, replaces and hides images.
    "visuals": {
        "project.toml": 'title = "Visuals"\nresolution = [1280, 720]\n',
        "scripts/start.lua": """\
label("start", function()
  scene("bg/classroom")
  show("eileen", { at = "left" })
  say("Eileen", "You came back.")
  show("eileen", { image = "eileen_happy", x = 0.5, y = 0.9 })
  hide("eileen")
  narrate("She left.")
end)
""",
    },
    # Shows an image that has no file, on line 3 of its only script.
    "missing_image": {
        "project.toml": 'title = "Missing image"\n',
        "scripts/start.lua": """\
label("start", function()
  narrate("before the error")
  show("ghost")
end)
""",
    },
    # Two scripts, a jump between them, a call and its return.
    "demo": {
        "project.toml": """\
title = "Demo"
version = "0.1.0"
resolution = [1280, 720]
languages = ["en"]
""",
        "scripts/start.lua": """\
local eileen = { name = "Eileen" }

label("start", function()
  narrate("It was raining.")
  say(eileen, "You came back.")
  call("aside")
  jump("ending")
end)

label("aside", function()
  say("Lucy", "Took you long enough.")
end)
""",
        "scripts/ending.lua": """\
label("ending", function()
  narrate("The end.")
end)
""",
    },
    # Never ends: the step limit stops it.
    "endless": {
        "project.toml": 'title = "Endless"\n',
        "scripts/start.lua": """\
label("start", function()
  while true do
    narrate("again")
  end
end)
""",
    },
    # Fails on line 3 of its only script.
    "broken": {
        "project.toml": 'title = "Broken"\n',
        "scripts/start.lua": """\
label("start", function()
  narrate("before the error")
  jump("nowhere")
end)
""",
    },
    # project.toml without the required title.
    "bad_config": {
        "project.toml": 'version = "1.0"\n',
        "scripts/start.lua": 'label("start", function() end)\n',
    },
    # Rolls dice with the engine RNG; every run gives the same numbers.
    "dice": {
        "project.toml": 'title = "Dice"\n',
        "scripts/start.lua": """\
label("start", function()
  for _ = 1, 5 do
    narrate("rolled " .. rand(1, 6))
  end
  narrate("drew " .. math.random(1000))
end)
""",
    },
    # Stops at a menu, which `vnmaki run --headless` cannot answer yet.
    "menu": {
        "project.toml": 'title = "Menu"\n',
        "scripts/start.lua": """\
label("start", function()
  menu({ "Stay.", "Leave." })
end)
""",
    },
    # Characters and a mix of say and narrate lines. It also has a font, so the windowed
    # `vnmaki run` can draw it (see FONT_PROJECTS).
    "say": {
        "project.toml": 'title = "Say"\nresolution = [1280, 720]\n',
        "scripts/start.lua": """\
local eileen = character { name = "Eileen", color = "#c8a2c8", voice_tag = "eileen" }

label("start", function()
  narrate("It was raining.")
  say(eileen, "You came back.")
  say(eileen, "I thought you would not.")
  say("???", "Who is there?")
end)
""",
    },
    # Ends at once, so a windowed run closes by itself.
    "finished": {
        "project.toml": 'title = "Finished"\n',
        "scripts/start.lua": 'label("start", function() end)\n',
    },
    # A script error with a font, so the windowed run gets as far as running the story.
    "broken_with_font": {
        "project.toml": 'title = "Broken"\n',
        "scripts/start.lua": """\
label("start", function()
  jump("nowhere")
end)
""",
    },
    # No project.toml at all.
    "no_config": {
        "scripts/start.lua": 'label("start", function() end)\n',
    },
}

# Image names each project can show: image name N is the file images/N.png.
IMAGES = {
    "visuals": ["bg/classroom", "eileen", "eileen_happy"],
}

# Projects that get the box font of text_fixtures.py as fonts/default.ttf, the font a windowed
# `vnmaki run` reads.
FONT_PROJECTS = ["say", "finished", "broken_with_font"]

# The headless run draws nothing, so one opaque pixel is enough.
PIXEL_PNG = png_rgba([[(128, 128, 128, 255)]])

TRANSCRIPTS = {
    "say": """\
label start
narrate It was raining.
say Eileen: You came back.
say Eileen: I thought you would not.
say ???: Who is there?
""",
    "visuals": """\
label start
scene bg/classroom
show eileen at left
say Eileen: You came back.
show eileen image eileen_happy x 0.5 y 0.9
hide eileen
narrate She left.
""",
    "demo": """\
label start
narrate It was raining.
say Eileen: You came back.
label aside
say Lucy: Took you long enough.
resume start
label ending
narrate The end.
""",
}


# The engine's default seed (kDefaultRandomSeed in script_runtime.hpp).
DEFAULT_SEED = 0x5EED0F7E570B5E
MASK = (1 << 64) - 1


class Rng:
    """The engine RNG: splitmix64, with rejection so every value is as likely."""

    def __init__(self, seed: int) -> None:
        self.state = seed

    def next(self) -> int:
        self.state = (self.state + 0x9E3779B97F4A7C15) & MASK
        z = self.state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & MASK
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & MASK
        return z ^ (z >> 31)

    def integer(self, low: int, high: int) -> int:
        count = high - low + 1
        threshold = (MASK + 1 - count) % count
        value = self.next()
        while value < threshold:
            value = self.next()
        return low + value % count


def dice_transcript() -> str:
    rng = Rng(DEFAULT_SEED)
    lines = ["label start"]
    lines += [f"narrate rolled {rng.integer(1, 6)}" for _ in range(5)]
    lines += [f"narrate drew {rng.integer(1, 1000)}"]
    return "\n".join(lines) + "\n"


TRANSCRIPTS["dice"] = dice_transcript()


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: runtime_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for project, files in PROJECTS.items():
        for relative, text in files.items():
            path = out / project / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(text.encode("utf-8"))
    for project, names in IMAGES.items():
        for name in names:
            path = out / project / "images" / f"{name}.png"
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(PIXEL_PNG)
    for project in FONT_PROJECTS:
        path = out / project / "fonts" / "default.ttf"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(text_fixtures.build_font())
    for project, text in TRANSCRIPTS.items():
        (out / f"{project}.transcript").write_bytes(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
