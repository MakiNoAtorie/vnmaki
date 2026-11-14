#!/usr/bin/env python3
"""Generate the fixtures of the camera GPU test (tests/render/camera_render_test.cpp).

Usage: camera_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/game/    a game project (32x32 virtual screen). Each case is a label: a backdrop on the
                   background layer, a sprite on the characters layer, a sprite on the overlay
                   layer and a sprite on the UI layer, then a camera call
  OUT_DIR/golden/  <case>.png: the frame, drawn by the reference rasterizer below

The reference does not use render_fixtures.Canvas.draw, which places whole-pixel rectangles. It
maps each screen pixel back through the camera to the layer, samples the layer's image with the
bilinear filter of the GPU (clamped at the edges) and blends it over the pixel as Canvas does.
The shake case repeats the noise of engine/scene/camera.cpp.
"""

import math
import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)

SIZE = rf.TARGET_SIZE
CENTER = SIZE / 2
SPRITE = [[rf.premultiply(pixel) for pixel in row] for row in rf.SPRITE]
BACKDROP = rf.BACKDROP

# Layers from the back: (image, left, top, parallax index). The sprites stand on their bottom
# centre: "eileen" at x 0.5, y 1 (left 12, top 16), "tree" at x 0.25, y 0.75 (left 4, top 8) and
# "hud" at x 0.75, y 0.5 (left 20, top 0).
SHOW = [
    ('show("eileen", { x = 0.5, y = 1 })'),
    ('show("tree", { image = "eileen", layer = "overlay", x = 0.25, y = 0.75 })'),
    ('show("hud", { image = "eileen", layer = "ui", x = 0.75, y = 0.5 })'),
]
BACKGROUND = ("background", BACKDROP, 0, 0)
CHARACTERS = ("characters", SPRITE, 12, 16)
OVERLAY = ("overlay", SPRITE, 4, 8)
HUD = ("ui", SPRITE, 20, 0)

# --- The shake noise (engine/scene/camera.cpp) -----------------------------------------------

MASK = 0xFFFFFFFF
SHAKE_RATE = 24.0


def lattice_value(point: int, axis: int) -> float:
    h = (point * 0x9E3779B1 + (axis + 1) * 0x85EBCA6B) & MASK
    h ^= h >> 15
    h = (h * 0x2C1B3C6D) & MASK
    h ^= h >> 12
    h = (h * 0x297A2D39) & MASK
    h ^= h >> 15
    return h / 4294967295.0 * 2 - 1


def shake_noise(seconds: float, axis: int) -> float:
    position = seconds * SHAKE_RATE
    whole = math.floor(position)
    fraction = position - whole
    smooth = fraction * fraction * (3 - 2 * fraction)
    a, b = lattice_value(whole, axis), lattice_value(whole + 1, axis)
    return a + (b - a) * smooth


def shake_offset(strength: float, duration: float, elapsed: float) -> tuple:
    if elapsed >= duration:
        return (0.0, 0.0)
    amplitude = strength * (1 - elapsed / duration)
    return (amplitude * shake_noise(elapsed, 0), amplitude * shake_noise(elapsed, 1))


# --- Drawing ---------------------------------------------------------------------------------


def bilinear(texture: list, tx: float, ty: float) -> list:
    """The premultiplied colour (0..1) at (tx, ty) texels from the top-left, clamped."""
    height, width = len(texture), len(texture[0])
    x0 = math.floor(tx - 0.5)
    y0 = math.floor(ty - 0.5)
    fx, fy = tx - 0.5 - x0, ty - 0.5 - y0

    def at(x: int, y: int) -> list:
        return [c / 255 for c in texture[min(height - 1, max(0, y))][min(width - 1, max(0, x))]]

    top = [a + (b - a) * fx for a, b in zip(at(x0, y0), at(x0 + 1, y0))]
    bottom = [a + (b - a) * fx for a, b in zip(at(x0, y0 + 1), at(x0 + 1, y0 + 1))]
    return [a + (b - a) * fy for a, b in zip(top, bottom)]


def draw_layer(canvas: rf.Canvas, layer: tuple, pan: tuple, zoom: float, rotation: float) -> None:
    """Draws an image of `layer` as the camera shows it. `pan` is this layer's pan in pixels."""
    _, texture, left, top = layer
    height, width = len(texture), len(texture[0])
    radians = math.radians(rotation)
    cosine, sine = math.cos(radians), math.sin(radians)
    for py in range(SIZE):
        for px in range(SIZE):
            # Back from the screen pixel to the layer: undo the zoom and the rotation about the
            # centre, then the pan.
            dx, dy = (px + 0.5 - CENTER) / zoom, (py + 0.5 - CENTER) / zoom
            wx = CENTER + pan[0] + dx * cosine + dy * sine
            wy = CENTER + pan[1] - dx * sine + dy * cosine
            tx, ty = wx - left, wy - top
            if not (0 <= tx < width and 0 <= ty < height):
                continue
            src = bilinear(texture, tx, ty)
            dst = [c / 255 for c in canvas.pixels[py][px]]
            out = [src[i] + dst[i] * (1 - src[3]) for i in range(4)]
            canvas.pixels[py][px] = tuple(round(c * 255) for c in out)


def frame(pan: tuple, zoom: float, rotation: float, parallax: dict) -> bytes:
    canvas = rf.Canvas(SIZE, SIZE, (0, 0, 0, 255))
    for layer in (BACKGROUND, CHARACTERS, OVERLAY):
        factor = parallax.get(layer[0], 1.0)
        draw_layer(canvas, layer, (pan[0] * factor, pan[1] * factor), zoom, rotation)
    draw_layer(canvas, HUD, (0, 0), 1.0, 0.0)  # the UI layer stays
    return canvas.png()


# Each case: (label, Lua that moves the camera, seconds the scene runs before the frame, then
# the frame's pan in pixels, zoom, rotation in degrees and the parallax of each layer).
SHAKE_STRENGTH = 8.0  # shake { strength = 0.25 } of a screen 32 high
SHAKE_DURATION = 1.0
SHAKE_AT = 0.3


def cases() -> list:
    shaken = shake_offset(SHAKE_STRENGTH, SHAKE_DURATION, SHAKE_AT)
    return [
        ("none", "", 0.0, (0, 0), 1.0, 0.0, {}),
        ("pan", "camera { x = 0.125, y = 0.0625 }", 0.0, (4, 2), 1.0, 0.0, {}),
        ("zoom", "camera { zoom = 2 }", 0.0, (0, 0), 2.0, 0.0, {}),
        ("rotation", "camera { rotation = 90 }", 0.0, (0, 0), 1.0, 90.0, {}),
        ("parallax",
         'layer("background", { parallax = 0.5 })\n'
         '  layer("overlay", { parallax = 2 })\n  camera { x = 0.25 }',
         0.0, (8, 0), 1.0, 0.0, {"background": 0.5, "overlay": 2.0}),
        ("shake", "shake { strength = 0.25, duration = 1 }", SHAKE_AT, shaken, 1.0, 0.0, {}),
    ]


def script() -> str:
    lines = []
    for label, lua, _, _, _, _, _ in cases():
        lines += ['label("%s", function()' % label, '  scene("bg/room")']
        lines += ["  " + line for line in SHOW]
        if lua:
            lines.append("  " + lua)
        lines += ['  narrate("framed")', "end)", ""]
    return "\n".join(lines)


def game() -> dict:
    return {
        "scripts/start.lua": script().encode("utf-8"),
        "images/bg/room.png": rf.png_rgba(rf.BACKDROP),
        "images/eileen.png": rf.png_rgba(rf.SPRITE),
    }


def goldens() -> dict:
    return {
        "%s.png" % label: frame(pan, zoom, rotation, parallax)
        for label, _, _, pan, zoom, rotation, parallax in cases()
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: camera_fixtures.py OUT_DIR", file=sys.stderr)
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
