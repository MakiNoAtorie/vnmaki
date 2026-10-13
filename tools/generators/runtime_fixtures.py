#!/usr/bin/env python3
"""Generate the game projects the `vnmaki run` CLI tests run.

Usage: runtime_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Each project is a folder with a project.toml and Lua scripts under scripts/.
For projects that run to the end, OUT_DIR/<name>.transcript holds the exact
stdout `vnmaki run <project> --headless` must print, written by hand from
the scripts below.
"""

import shutil
import sys
from pathlib import Path

PROJECTS = {
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
    # No project.toml at all.
    "no_config": {
        "scripts/start.lua": 'label("start", function() end)\n',
    },
}

TRANSCRIPTS = {
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
    for project, text in TRANSCRIPTS.items():
        (out / f"{project}.transcript").write_bytes(text.encode("utf-8"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
