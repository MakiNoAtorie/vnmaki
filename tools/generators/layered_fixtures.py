#!/usr/bin/env python3
"""Generate the fixtures of the layered sprite GPU test (tests/render/layered_render_test.cpp).

Usage: layered_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/game/    a game project: a layered sprite "eileen" (base, outfit, face) shown on a
                   backdrop, and then its face changed
  OUT_DIR/golden/  what the two frames must look like, drawn by the reference rasterizer of
                   render_fixtures.py

The sprite stands at the bottom centre of a 32x32 scene: its 8x16 base covers x 12..20 and
y 16..32. The face images are strips 8x4 at the top of the sprite.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)

BODY_WIDTH = 8
BODY_HEIGHT = 16
FACE_HEIGHT = 4
CLEAR = (0, 0, 0, 0)
SKIN = (224, 172, 140, 255)
SCHOOL = (40, 60, 160, 255)
SCHOOL_HEM = (40, 60, 160, 128)  # Half clear, so the blend shows.
CASUAL = (200, 60, 60, 255)
EYE = (20, 20, 20, 255)
SMILE = (180, 40, 40, 255)
FROWN = (40, 40, 180, 255)

# The fade 0.1 s into a crossfade of 0.25 s: 0.4 of 255.
FADE_ALPHA = 102
CROSSFADE_SECONDS = 0.25


def body_pixel(x: int, y: int) -> tuple:
    return SKIN if 1 <= x < BODY_WIDTH - 1 else CLEAR


def outfit(color: tuple) -> list:
    """Everything below the head; the last row is half clear."""
    def pixel(x: int, y: int) -> tuple:
        if y < FACE_HEIGHT:
            return CLEAR
        return SCHOOL_HEM if (color == SCHOOL and y == BODY_HEIGHT - 1) else color
    return [[pixel(x, y) for x in range(BODY_WIDTH)] for y in range(BODY_HEIGHT)]


def face(mouth_row: int, mouth: tuple) -> list:
    """Two eyes on row 1 and a mouth four pixels wide."""
    def pixel(x: int, y: int) -> tuple:
        if y == 1 and x in (2, BODY_WIDTH - 3):
            return EYE
        if y == mouth_row and 2 <= x < BODY_WIDTH - 2:
            return mouth
        return CLEAR
    return [[pixel(x, y) for x in range(BODY_WIDTH)] for y in range(FACE_HEIGHT)]


BASE = [[body_pixel(x, y) for x in range(BODY_WIDTH)] for y in range(BODY_HEIGHT)]
OUTFIT_SCHOOL = outfit(SCHOOL)
OUTFIT_CASUAL = outfit(CASUAL)
FACE_SMILE = face(3, SMILE)
FACE_SAD = face(2, FROWN)

SCRIPT = """\
layered("eileen", {
  order = { "base", "outfit", "face" },
  base = "eileen/base",
  outfit = { school = "eileen/outfit_school", casual = "eileen/outfit_casual" },
  face = { smile = "eileen/face_smile", sad = "eileen/face_sad" },
  default = { outfit = "school", face = "smile" },
  crossfade = %s,
})

label("start", function()
  scene("bg/room")
  show("eileen")
  narrate("composed")
  show("eileen", { face = "sad" })
  narrate("fading")
end)
""" % CROSSFADE_SECONDS


def premultiplied(pixels: list) -> list:
    return [[rf.premultiply(pixel) for pixel in row] for row in pixels]


def game() -> dict:
    return {
        "scripts/start.lua": SCRIPT.encode("utf-8"),
        "images/bg/room.png": rf.png_rgba(rf.BACKDROP),
        "images/eileen/base.png": rf.png_rgba(BASE),
        "images/eileen/outfit_school.png": rf.png_rgba(OUTFIT_SCHOOL),
        "images/eileen/outfit_casual.png": rf.png_rgba(OUTFIT_CASUAL),
        "images/eileen/face_smile.png": rf.png_rgba(FACE_SMILE),
        "images/eileen/face_sad.png": rf.png_rgba(FACE_SAD),
    }


def goldens() -> dict:
    origin_x = rf.TARGET_SIZE // 2 - BODY_WIDTH // 2
    origin_y = rf.TARGET_SIZE - BODY_HEIGHT
    body = (origin_x, origin_y, BODY_WIDTH, BODY_HEIGHT)
    strip = (origin_x, origin_y, BODY_WIDTH, FACE_HEIGHT)

    def composed() -> rf.Canvas:
        canvas = rf.Canvas(rf.TARGET_SIZE, rf.TARGET_SIZE, (0, 0, 0, 255))
        canvas.draw(rf.BACKDROP, (0, 0, rf.TARGET_SIZE, rf.TARGET_SIZE))
        canvas.draw(premultiplied(BASE), body)
        canvas.draw(premultiplied(OUTFIT_SCHOOL), body)
        canvas.draw(premultiplied(FACE_SMILE), strip)
        return canvas

    scenes = {"layered_composed": composed()}
    # The new face fades in over the old one, which stays whole.
    fading = composed()
    fading.draw(premultiplied(FACE_SAD), strip, tint=(255, 255, 255, FADE_ALPHA))
    scenes["layered_crossfade"] = fading
    return {name + ".png": canvas.png() for name, canvas in scenes.items()}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: layered_fixtures.py OUT_DIR", file=sys.stderr)
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
