#!/usr/bin/env python3
"""Generate the goldens of the presentation demo (tests/render/presentation_render_test.cpp).

Usage: presentation_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. It gets golden/<name>.png: three frames of the label
"presentation" in templates/default/ (1280x720), drawn by the reference rasterizer of
render_fixtures.py (Canvas, the PNG writer) from the pictures that template_assets.py makes.

The test plays the real script and moves a fake clock (Scene::update). Seconds are binary
fractions, so the clock is exact. The frames, with the clock in seconds since the sprite was shown:

  presentation_face    0.125: halfway through the 0.25 s crossfade from the smile to the sad face.
  presentation_camera  1.9375: the pan of the camera has ended (80 pixels, on the characters layer
                       only: the background has parallax 0), the shake has ended, and the blink
                       animation (8 fps, frame 15 of 17) shows the closed eyelids.
  presentation_sepia   2.4375: the sepia shader of the background layer at amount 0.5, with the
                       camera still panned and the eyelids open (frame 2).

The frames with a shake, a dissolve or a mask are not here: the shake moves by fractions of a
pixel (a bilinear filter), and a transition blends the old frame with the new. Those are checked
by camera_fixtures.py and transition_fixtures.py on small scenes, where the reference is cheap.
The lip sync and the movie follow the audio clock, so tests/unit/new_project_test.cpp checks them.
Only the standard library is used.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)
import shader_fixtures as sf  # noqa: E402  (the reference of the sepia shader)
import template_assets as ta  # noqa: E402  (the pictures of the template)

WIDTH, HEIGHT = ta.BACKDROP_SIZE
SPRITE_WIDTH, SPRITE_HEIGHT = ta.SPRITE_SIZE
SPRITE_LEFT = WIDTH // 2 - SPRITE_WIDTH // 2  # shown at "center": bottom centre of the screen
SPRITE_TOP = HEIGHT - SPRITE_HEIGHT
PAN = 80  # camera { x = 0.0625 } is 0.0625 of the width
CROSSFADE_ALPHA = 0.5  # 0.125 s of 0.25 s
SEPIA_AMOUNT = 0.5  # 0.5 s of a tween from 0 to 1 over 1 s


def rows_of(image: ta.Image) -> list:
    """The rows of RGBA tuples of an image; its alpha is 0 or 255, so straight is premultiplied."""
    return [list(zip(r[0::4], r[1::4], r[2::4], r[3::4])) for r in image.rows]


def blit(canvas: rf.Canvas, rows: list, left: int, top: int, alpha: float = 1.0) -> None:
    """The same blend as Canvas.draw for a whole-pixel quad, with alpha as the tint's alpha."""
    for y, row in enumerate(rows):
        py = top + y
        if not 0 <= py < canvas.height:
            continue
        target = canvas.pixels[py]
        for x, pixel in enumerate(row):
            px = left + x
            if pixel[3] == 0 or not 0 <= px < canvas.width:
                continue
            if alpha == 1.0:
                target[px] = pixel
            else:
                dst = target[px]
                target[px] = tuple(round(pixel[i] * alpha + dst[i] * (1 - alpha))
                                   for i in range(4))


def backdrop(shade=None) -> rf.Canvas:
    """The background layer. A shader is applied to each color once (the backdrop is flat)."""
    canvas = rf.Canvas(WIDTH, HEIGHT, (0, 0, 0, 255))
    cache = {}

    def toned(pixel: tuple) -> tuple:
        if pixel not in cache:
            out = shade([c / 255 for c in pixel])
            cache[pixel] = tuple(round(c * 255) for c in out)
        return cache[pixel]

    for y, row in enumerate(rows_of(ta.rooftop())):
        canvas.pixels[y] = [toned(p) for p in row] if shade else row
    return canvas


def frame(fade: bool, closed_eyes: bool, pan: int, shade=None) -> bytes:
    canvas = backdrop(shade)
    left = SPRITE_LEFT - pan
    blit(canvas, rows_of(ta.sprite_base()), left, SPRITE_TOP)
    blit(canvas, rows_of(ta.sprite_outfit()), left, SPRITE_TOP)
    if fade:
        # The new face fades in over the old one, which stays whole.
        blit(canvas, rows_of(ta.sprite_face(True)), left, SPRITE_TOP)
        blit(canvas, rows_of(ta.sprite_face(False)), left, SPRITE_TOP, CROSSFADE_ALPHA)
    else:
        blit(canvas, rows_of(ta.sprite_face(False)), left, SPRITE_TOP)
    blit(canvas, rows_of(ta.sprite_lids(2 if closed_eyes else 0)), left, SPRITE_TOP)
    blit(canvas, rows_of(ta.sprite_mouth(0)), left, SPRITE_TOP)  # lip sync: closed, no voice
    return canvas.png()


def goldens() -> dict:
    return {
        "presentation_face.png": frame(fade=True, closed_eyes=False, pan=0),
        "presentation_camera.png": frame(fade=False, closed_eyes=True, pan=PAN),
        "presentation_sepia.png": frame(fade=False, closed_eyes=False, pan=PAN,
                                        shade=sf.sepia(SEPIA_AMOUNT)),
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: presentation_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for name, content in goldens().items():
        path = out / "golden" / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
