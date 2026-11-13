#!/usr/bin/env python3
"""Generate the fixtures of the transition GPU test (tests/render/transition_render_test.cpp).

Usage: transition_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/game/    a game project (32x32 virtual screen). Each case is a label: the old scene is
                   a backdrop and a sprite in the middle (and a sprite on the UI layer), then one
                   step changes the backdrop and moves the sprite to the left with
                   `with = { <transition>, duration = 1, ... }`
  OUT_DIR/golden/  <case>_<stage>.png: the frame at progress 0, 0.25, 0.5 and 1 (stage 0, 25,
                   50, 100), drawn by the reference rasterizer of render_fixtures.py

The UI sprite is not part of the transition: it is drawn over the mix in every frame.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)

SIZE = rf.TARGET_SIZE
STAGES = (("0", 0.0), ("25", 0.25), ("50", 0.5), ("100", 1.0))
FADE_COLOR = (1.0, 0.5, 0.0)
SOFTNESS = 0.25
QUICK_AMOUNT = 2.0  # the custom shader finishes at half the time

NEW_BACKDROP = [[(255 - x * 8, 128, y * 8, 255) for x in range(SIZE)] for y in range(SIZE)]
MASK = [[(x * 8, x * 8, x * 8, 255) for x in range(SIZE)] for _ in range(SIZE)]

QUICK_SHADER = """\
#include "transition.hlsli"

cbuffer QuickUniforms : register(b0, space3) {
  float progress;
  float amount;
  float2 unused_padding;
};

float4 main(SpriteFragment input) : SV_Target0 {
  return lerp(from_at(input.uv), to_at(input.uv), saturate(progress * amount));
}
"""


def lerp(a: float, b: float, t: float) -> float:
    return a + (b - a) * t


def saturate(x: float) -> float:
    return min(1.0, max(0.0, x))


def reveal(position: float, progress: float, softness: float) -> float:
    if progress <= 0.0:
        return 0.0
    if progress >= 1.0:
        return 1.0
    return saturate((progress * (1.0 + softness) - position) / max(softness, 0.000001))


def mix_lerp(a: list, b: list, t: float) -> list:
    return [lerp(a[i], b[i], t) for i in range(4)]


# Each case: (label, `with` options in Lua, a function (old, new, x, y, progress) -> floats).
# `old` and `new` are the frames (rows of premultiplied RGBA8); x and y are the pixel.


def texel(frame: list, u: float, v: float) -> list:
    """The nearest texel at (u, v), or transparent outside the texture."""
    if not (0.0 <= u <= 1.0 and 0.0 <= v <= 1.0):
        return [0.0, 0.0, 0.0, 0.0]
    x = min(SIZE - 1, int(u * SIZE))
    y = min(SIZE - 1, int(v * SIZE))
    return [c / 255 for c in frame[y][x]]


def at(frame: list, x: int, y: int) -> list:
    return [c / 255 for c in frame[y][x]]


def dissolve(old, new, x, y, p):
    return mix_lerp(at(old, x, y), at(new, x, y), p)


def fade(old, new, x, y, p):
    through = [FADE_COLOR[0], FADE_COLOR[1], FADE_COLOR[2], 1.0]
    going = mix_lerp(at(old, x, y), through, saturate(p * 2))
    coming = mix_lerp(through, at(new, x, y), saturate(p * 2 - 1))
    return going if p < 0.5 else coming


def slide(direction: tuple):
    def mix(old, new, x, y, p):
        u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
        a = texel(old, u - direction[0] * p, v - direction[1] * p)
        b = texel(new, u + direction[0] * (1 - p), v + direction[1] * (1 - p))
        return [a[i] + b[i] for i in range(4)]
    return mix


def wipe(direction: tuple, softness: float):
    def mix(old, new, x, y, p):
        u, v = (x + 0.5) / SIZE, (y + 0.5) / SIZE
        position = 0.5 + (u - 0.5) * direction[0] + (v - 0.5) * direction[1]
        return mix_lerp(at(old, x, y), at(new, x, y), reveal(position, p, softness))
    return mix


def mask(old, new, x, y, p):
    position = MASK[y][x][0] / 255
    return mix_lerp(at(old, x, y), at(new, x, y), reveal(position, p, SOFTNESS))


def quick(old, new, x, y, p):
    return mix_lerp(at(old, x, y), at(new, x, y), saturate(p * QUICK_AMOUNT))


CASES = [
    ("dissolve", '"dissolve"', dissolve),
    ("fade", '"fade", color = "#ff8000"', fade),
    ("slide_left", '"slide_left"', slide((-1, 0))),
    ("slide_right", '"slide_right"', slide((1, 0))),
    ("slide_up", '"slide_up"', slide((0, -1))),
    ("slide_down", '"slide_down"', slide((0, 1))),
    ("wipe_left", '"wipe_left", softness = 0.25', wipe((-1, 0), SOFTNESS)),
    ("wipe_right", '"wipe_right"', wipe((1, 0), 0.0)),
    ("wipe_up", '"wipe_up", softness = 0.25', wipe((0, -1), SOFTNESS)),
    ("wipe_down", '"wipe_down"', wipe((0, 1), 0.0)),
    ("mask", '"mask", mask = "masks/gradient", softness = 0.25', mask),
    ("custom", '"quick", uniforms = { amount = 2 }', quick),
]

SPRITE = [[rf.premultiply(pixel) for pixel in row] for row in rf.SPRITE]
OLD_PLACE = (12, 16, rf.SPRITE_WIDTH, rf.SPRITE_HEIGHT)  # at the centre
NEW_PLACE = (4, 16, rf.SPRITE_WIDTH, rf.SPRITE_HEIGHT)  # x = 0.25
UI_PLACE = (20, 0, rf.SPRITE_WIDTH, rf.SPRITE_HEIGHT)  # x = 0.75, y = 0.5


def script() -> str:
    lines = ['shader("quick", { "progress", "amount" })', ""]
    for label, options, _ in CASES:
        lines += [
            'label("%s", function()' % label,
            '  scene("bg/room")',
            '  show("eileen")',
            '  show("hud", { image = "eileen", layer = "ui", x = 0.75, y = 0.5 })',
            '  narrate("before")',
            '  scene("bg/night")',
            '  show("lucy", { image = "eileen", x = 0.25, y = 1, with = { %s, duration = 1 } })'
            % options,
            '  narrate("after")',
            "end)",
            "",
        ]
    return "\n".join(lines)


def game() -> dict:
    return {
        "scripts/start.lua": script().encode("utf-8"),
        "images/bg/room.png": rf.png_rgba(rf.BACKDROP),
        "images/bg/night.png": rf.png_rgba(NEW_BACKDROP),
        "images/eileen.png": rf.png_rgba(rf.SPRITE),
        "images/masks/gradient.png": rf.png_rgba(MASK),
        "shaders/quick.frag.hlsl": QUICK_SHADER.encode("utf-8"),
    }


def frame(backdrop: list, place: tuple) -> list:
    canvas = rf.Canvas(SIZE, SIZE, (0, 0, 0, 255))
    canvas.draw(backdrop, (0, 0, SIZE, SIZE))
    canvas.draw(SPRITE, place)
    return canvas.pixels


def goldens() -> dict:
    old = frame(rf.BACKDROP, OLD_PLACE)
    new = frame(NEW_BACKDROP, NEW_PLACE)
    files = {}
    for label, _, mixer in CASES:
        for stage, progress in STAGES:
            canvas = rf.Canvas(SIZE, SIZE, (0, 0, 0, 255))
            for y in range(SIZE):
                for x in range(SIZE):
                    mixed = mixer(old, new, x, y, progress)
                    canvas.pixels[y][x] = tuple(round(saturate(c) * 255) for c in mixed)
            canvas.draw(SPRITE, UI_PLACE)
            files["%s_%s.png" % (label, stage)] = canvas.png()
    return files


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: transition_fixtures.py OUT_DIR", file=sys.stderr)
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
