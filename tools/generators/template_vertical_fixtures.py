#!/usr/bin/env python3
"""Generate the golden of the template say box in vertical Japanese
(tests/render/template_vertical_render_test.cpp, docs/PLAN.md task 53).

Usage: template_vertical_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. It gets golden/say_vertical.png (1280x720), drawn by the
reference rasterizer of render_fixtures.py. Nothing here reads the engine's layout: the numbers
below are worked out from the default `say` screen (engine/runtime/default_screens.cpp) for a
canvas of 1280 by 720, for a narrated line with `[vertical] ja = true`, and from the pixel font of
the template (template_assets.py).

The box. margin = 1280 // 16 = 80 and pad = 1280 // 64 = 20. The vertical box is 720 * 5 // 12 =
300 high and sits 720 // 24 = 30 above the bottom: (80, 390, 1120, 300), colour #101020cc. The
name line is 720 // 32 = 22 high, so the line starts at y = 20 + 22 + 10 = 52 in the box (442) and
has the width 1120 - 40 = 1080 and the height 300 - 52 - 20 = 228. A narration has no name, so
nothing else is drawn. The size of the line is 720 // 24 = 30.

The line is "おわり。あなたの物語を書こう。" (the sample of translations/ja.po). A column is 228 / 30 = 7
glyphs long. The first seven glyphs fill column 1 ("おわり。あなた"), the next seven column 2
("の物語を書こう"), and "。" may not start a column, so "う" goes down with it: column 2 has
"の物語を書こ" and column 3 has "う。". The columns hug the right edge of the text area (100 +
1080 = 1180), the first at the right, each 30 thick.

What the golden proves. The pixel font of the template has no Japanese letters, so every glyph of
the line is the .notdef box (one rectangle, 50 to 450 by 0 to 700 units, advance 500). The golden
therefore proves the layout and not the shaping: the language switch ([vertical] and the sample
translation reaching the screen), the vertical flag of the say style, the box size and place, the
column length, the kinsoku move of "う", the columns from the right, the upright glyphs centred on
the column, and the typewriter count (all 15 glyphs). It does not prove the shapes of kana, the
`vert` forms (the font has no GSUB), or ruby.

The rasterizer. At 30 pixels per em a font unit is 0.03 pixels, so the box is 1.5 to 13.5 pixels
across and 0 to 21 pixels up from the baseline (24 under the top of the cell). The glyph is
centred by its advance, so its origin is 7.5 pixels left of the column centre, and the engine
rounds the origin (half away from zero) to the pixel on its right. The left and right edges of
the box are then half a pixel, and those pixels get half coverage. The coverage of each pixel is
the exact area, as the rasterizer of FreeType gives it; the test accepts the default tolerance of 2
for its rounding.
"""

import math
import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)
import text_fixtures as tf  # noqa: E402  (the units of the box font)

WIDTH, HEIGHT = 1280, 720
BACKDROP = (26, 26, 26, 255)
PANEL = (80, 390, 1120, 300)
PANEL_TINT = (16, 16, 32, 204)
WHITE = (255, 255, 255, 255)

SIZE = 30
RIGHT = 100 + 1080  # the right edge of the text area
TOP = 390 + 52
COLUMNS = ["おわり。あなた", "の物語を書こ", "う。"]

# .notdef of fonts/default.ttf is text_fixtures.UPPER: advance 500, box 50..450 by 0..700.
ADVANCE, (X0, Y0, X1, Y1) = tf.UPPER
UNIT = SIZE / tf.UNITS_PER_EM  # pixels of a font unit
ASCENT = tf.ASCENT * UNIT


def overlap(low: float, high: float, pixel: int) -> float:
    return max(0.0, min(high, pixel + 1) - max(low, pixel))


def fill(canvas: rf.Canvas, left: float, top: float, right: float, bottom: float,
         tint: tuple) -> None:
    """Draws a rectangle with edges between pixels; a pixel gets the area that it covers."""
    alpha = tint[3] / 255
    colour = [tint[0] / 255 * alpha, tint[1] / 255 * alpha, tint[2] / 255 * alpha, alpha]
    for py in range(math.floor(top), math.ceil(bottom)):
        for px in range(math.floor(left), math.ceil(right)):
            coverage = overlap(left, right, px) * overlap(top, bottom, py)
            if coverage <= 0:
                continue
            src = [c * coverage for c in colour]
            dst = [c / 255 for c in canvas.pixels[py][px]]
            canvas.pixels[py][px] = tuple(
                round((src[i] + dst[i] * (1 - src[3])) * 255) for i in range(4))


def round_half_away(value: float) -> int:
    return int(math.floor(abs(value) + 0.5)) * (1 if value >= 0 else -1)


def draw_say_vertical(canvas: rf.Canvas) -> None:
    canvas.draw(rf.WHITE, PANEL, tint=PANEL_TINT)
    for index, column in enumerate(COLUMNS):
        centre = RIGHT - SIZE * index - SIZE / 2
        pen = TOP
        for _ in column:
            # Upright: the glyph is centred by its advance, and the cell's top is at the pen.
            origin_x = round_half_away(centre - ADVANCE * UNIT / 2)
            origin_y = round_half_away(pen + ASCENT)
            fill(canvas, origin_x + X0 * UNIT, origin_y - Y1 * UNIT, origin_x + X1 * UNIT,
                 origin_y - Y0 * UNIT, WHITE)
            pen += SIZE  # one em down


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: template_vertical_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    (out / "golden").mkdir(parents=True)
    canvas = rf.Canvas(WIDTH, HEIGHT, BACKDROP)
    draw_say_vertical(canvas)
    (out / "golden" / "say_vertical.png").write_bytes(canvas.png())
    return 0


if __name__ == "__main__":
    sys.exit(main())
