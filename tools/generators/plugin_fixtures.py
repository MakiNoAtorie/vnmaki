#!/usr/bin/env python3
"""Generate the fixtures of the plugin tests (task 48, docs/plugins.md).

Usage: plugin_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/lua_ok/        a game with the Lua plugin "flash" (a transition and an API function)
  OUT_DIR/lua_ok.txt     the transcript `vnmaki run lua_ok --headless` must print
  OUT_DIR/lua_mismatch/  the same game; the plugin needs an engine that does not exist yet
  OUT_DIR/cpp/           a game that uses the C++ example plugin (plugins/example/): two Lua
  OUT_DIR/cpp.txt        functions, a displayable and a .ppm picture. It runs only in a build that
                         links the plugin (-DVNMAKI_PLUGINS=example); cpp.txt is its transcript
  OUT_DIR/game/          the 32 by 32 game of tests/render/plugin_render_test.cpp: the badge
                         displayable and a .ppm picture
  OUT_DIR/golden/plugin_frame.png  its frame, drawn by the reference rasterizer of render_fixtures

The PPM files are written here with the standard library only. The expected frame needs no
rasterizer of its own: the badge is a whole-pixel square of one colour and the picture is drawn
at scale 1, so rf.Canvas.draw places both exactly.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)

PICTURE_SIZE = 4
BADGE_SIZE = 12
BADGE_COLOR = (255, 136, 0, 255)  # "#ff8800"


def picture_pixel(x: int, y: int) -> tuple:
    return (10 + 60 * x, 10 + 60 * y, 128, 255)


def picture_ppm() -> bytes:
    body = bytearray()
    for y in range(PICTURE_SIZE):
        for x in range(PICTURE_SIZE):
            body += bytes(picture_pixel(x, y)[:3])
    return b"P6\n# a test picture\n%d %d\n255\n" % (PICTURE_SIZE, PICTURE_SIZE) + bytes(body)


def write(root: Path, files: dict) -> None:
    for name, data in files.items():
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if isinstance(data, bytes) else data.encode("utf-8"))


FLASH_LUA = """-- A white flash: the "fade" transition through white, a bit quicker than the default.
define_transition("flash", "fade", { color = "#ffffff", duration = 0.25 })

function shout(text)
  return string.upper(text) .. "!"
end
"""

LUA_MAIN = """label("start", function()
  transition("flash")
  narrate(shout("hi"))
end)
"""

LUA_TRANSCRIPT = "label start\ntransition flash\nwait @transition\nnarrate HI!\n"


def lua_game(engine_range: str) -> dict:
    return {
        "project.toml": 'title = "Lua plugin"\nplugins = ["flash"]\n',
        "plugins/flash/plugin.toml":
            f'name = "flash"\nversion = "1.0.0"\nengine = "{engine_range}"\n',
        "plugins/flash/flash.lua": FLASH_LUA,
        "scripts/main.lua": LUA_MAIN,
    }


CPP_MAIN = """label("start", function()
  narrate(example_greet("Maki"))
  narrate("sum " .. example_add(2, 3))
  show("badge", { displayable = "example_badge", params = { size = 20, color = "#336699" } })
  show("pic", { image = "pic" })
  narrate("done")
end)
"""

CPP_TRANSCRIPT = ("label start\nnarrate Hello, Maki!\nnarrate sum 5\n"
                  "show badge displayable example_badge\nshow pic image pic\nnarrate done\n")

GAME_MAIN = """label("start", function()
  show("badge", { displayable = "example_badge", params = { size = 12, color = "#ff8800" },
                  x = 0.5, y = 0.5 })
  show("pic", { image = "pic", x = 0.25, y = 1 })
  narrate("composed")
end)
"""


def golden_frame() -> bytes:
    canvas = rf.Canvas(rf.TARGET_SIZE, rf.TARGET_SIZE, (0, 0, 0, 255))
    # The badge stands on its bottom centre, (16, 16): left 10, top 4.
    canvas.draw(rf.WHITE, (16 - BADGE_SIZE // 2, 16 - BADGE_SIZE, BADGE_SIZE, BADGE_SIZE),
                tint=BADGE_COLOR)
    # The picture stands on (8, 32): left 6, top 28.
    texture = [[picture_pixel(x, y) for x in range(PICTURE_SIZE)] for y in range(PICTURE_SIZE)]
    canvas.draw(texture, (8 - PICTURE_SIZE // 2, 32 - PICTURE_SIZE, PICTURE_SIZE, PICTURE_SIZE))
    return canvas.png()


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    out = Path(sys.argv[1])
    if out.exists():
        shutil.rmtree(out)
    write(out / "lua_ok", lua_game(">=0.1.0"))
    write(out / "lua_mismatch", lua_game(">=9.0.0"))
    write(out, {"lua_ok.txt": LUA_TRANSCRIPT, "cpp.txt": CPP_TRANSCRIPT})
    write(out / "cpp", {
        "project.toml": 'title = "C++ plugin"\n',
        "scripts/main.lua": CPP_MAIN,
        "images/pic.ppm": picture_ppm(),
    })
    write(out / "game", {
        "scripts/start.lua": GAME_MAIN,
        "images/pic.ppm": picture_ppm(),
    })
    write(out / "golden", {"plugin_frame.png": golden_frame()})
    return 0


if __name__ == "__main__":
    sys.exit(main())
