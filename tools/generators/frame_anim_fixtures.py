#!/usr/bin/env python3
"""Generate the fixtures of the frame animation GPU test (tests/render/frame_anim_render_test.cpp).

Usage: frame_anim_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/game/    a game project: the animation "spark", made from a sheet of 2 x 2 cells of 8x8
                   pixels at 4 frames per second, shown on a backdrop
  OUT_DIR/golden/  what the frame at 0.8 s (the fourth cell, bottom right) must look like, drawn
                   by the reference rasterizer of render_fixtures.py

The animation stands at the bottom centre of a 32x32 scene: its 8x8 frame covers x 12..20 and
y 24..32.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)

CELL = 8
COLUMNS = 2
ROWS = 2
FPS = 4
SHOWN_CELL = 3  # The frame at 0.8 s: 0.8 s * 4 fps = 3.2.
CELL_COLORS = [(200, 40, 40, 255), (40, 160, 40, 255), (40, 60, 200, 255), (220, 200, 40, 255)]
MARK = (255, 255, 255, 255)

SCRIPT = """\
animation("spark", { sheet = "fx/spark", columns = %d, rows = %d, fps = %d })

label("start", function()
  scene("bg/room")
  show("spark")
  narrate("playing")
end)
""" % (COLUMNS, ROWS, FPS)


def cell_pixels(index: int) -> list:
    """A cell of one colour with a white diagonal, so a wrong cell or a flip shows."""
    color = CELL_COLORS[index]
    return [[MARK if x == y else color for x in range(CELL)] for y in range(CELL)]


def sheet() -> list:
    pixels = []
    for row in range(ROWS):
        for y in range(CELL):
            line = []
            for column in range(COLUMNS):
                line.extend(cell_pixels(row * COLUMNS + column)[y])
            pixels.append(line)
    return pixels


def game() -> dict:
    return {
        "scripts/start.lua": SCRIPT.encode("utf-8"),
        "images/bg/room.png": rf.png_rgba(rf.BACKDROP),
        "images/fx/spark.png": rf.png_rgba(sheet()),
    }


def goldens() -> dict:
    canvas = rf.Canvas(rf.TARGET_SIZE, rf.TARGET_SIZE, (0, 0, 0, 255))
    canvas.draw(rf.BACKDROP, (0, 0, rf.TARGET_SIZE, rf.TARGET_SIZE))
    frame = [[rf.premultiply(pixel) for pixel in row] for row in cell_pixels(SHOWN_CELL)]
    canvas.draw(frame, (rf.TARGET_SIZE // 2 - CELL // 2, rf.TARGET_SIZE - CELL, CELL, CELL))
    return {"frame_anim_sheet.png": canvas.png()}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: frame_anim_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for directory, files in (("game", game()), ("golden", goldens())):
        for name, content in files.items():
            path = out / directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
