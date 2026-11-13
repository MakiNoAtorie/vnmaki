#!/usr/bin/env python3
"""Generate the goldens of the content demo (tests/render/content_render_test.cpp).

Usage: content_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. It gets golden/<name>.png: three frames of the label "content"
in templates/default/ (1280x720), drawn by the reference rasterizer of render_fixtures.py (Canvas,
the PNG writer) from the classroom backdrop of template_assets.py and the arm of model_fixtures.py.

The test plays the real script and moves a fake clock (Scene::update). Seconds are binary
fractions, so the clock is exact. The frames:

  content_rest   The arm (models/arm.gltf, scale 2) at rest, before the clip. The camera is at
                 (0, 2, 6), looks at (0, 2, 0) with a field of view of 90 degrees, so a point at
                 depth d and offset (x, y) from the axis lands (x, y) * 360 / d pixels from the
                 centre of the screen. The unlit shader paints the orange of the material.
  content_slide  0.5 s into the clip "slide": the root joint, and so the whole bar, has moved 1
                 along x (the cubic spline of the clip is exactly 1 at its middle), 2 after the
                 scale of the model.
  content_flash  0.25 s into the transition "flash" of the plugin (the fade through white, 1 s):
                 progress 0.25, so the old frame (the classroom, nothing else is shown) is mixed
                 half way to white. The new frame does not show before progress 0.5.

Why these frames. The reference has no shading model and no depth buffer, so a frame must be a
flat colour: the unlit arm is one colour, and every triangle of it is projected and filled at the
pixel centres (the GPU has no multisampling here). The arm is moved as a whole, so its outline
has only a few edges. A bent pose (the clip "bend") has the slanted edges of the blended middle
ring, and along such an edge some pixel centres fall within the 1/256 pixel that a GPU snaps
vertices to, so the reference could not be exact there. The script checks that no pixel centre is
within 0.01 pixel of the outline. The toon and PBR shaders, the sprite (its blink and lip sync
follow the clock and the audio) and a frame with the shake are not here: model_fixtures.py,
presentation_fixtures.py and camera_fixtures.py check those on small scenes. Only the standard
library is used.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import model_fixtures as mf  # noqa: E402  (the arm and its colour)
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)
import template_assets as ta  # noqa: E402  (the backdrop)

WIDTH, HEIGHT = ta.BACKDROP_SIZE
CENTRE = (WIDTH / 2, HEIGHT / 2)
FOCAL = HEIGHT / 2  # a vertical field of view of 90 degrees: tan 45 = 1
CAMERA = (0.0, 2.0, 6.0)  # looks along -z
SCALE = 2.0  # model("arm", ..., { scale = 2 })
ORANGE = tuple(round(c * 255) for c in mf.ORANGE) + (255,)
SLIDE = 1.0  # the clip "slide" at 0.5 s, in the units of the model
FLASH_PROGRESS = 0.25
GUARD = 0.01  # pixels: how close to an edge a pixel centre may be


def arm_vertices(shift: float) -> list:
    """The 12 vertices of the bar (mf.skinned), moved `shift` along x by the root joint."""
    return [(x + shift, y, z) for y in mf.BAR_RINGS for x, z in mf.CORNERS]


def project(vertex: tuple) -> tuple:
    x, y, z = (c * SCALE for c in vertex)
    depth = CAMERA[2] - z
    return (CENTRE[0] + FOCAL * (x - CAMERA[0]) / depth,
            CENTRE[1] - FOCAL * (y - CAMERA[1]) / depth)


def triangles(vertices: list) -> list:
    """The side triangles of the bar, as mf.skinned() indexes them, projected to the screen."""
    points = [project(v) for v in vertices]
    out = []
    for segment in range(2):
        for corner in range(4):
            a, b = segment * 4 + corner, segment * 4 + (corner + 1) % 4
            out.append((points[a], points[b], points[b + 4]))
            out.append((points[a], points[b + 4], points[a + 4]))
    return out


def inside(triangle: tuple, x: float, y: float) -> bool:
    (ax, ay), (bx, by), (cx, cy) = triangle
    d1 = (bx - ax) * (y - ay) - (by - ay) * (x - ax)
    d2 = (cx - bx) * (y - by) - (cy - by) * (x - bx)
    d3 = (ax - cx) * (y - cy) - (ay - cy) * (x - cx)
    return (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0)


def covered(shapes: list, x: float, y: float) -> bool:
    return any(inside(t, x, y) for t in shapes)


def coverage(shapes: list) -> list:
    """The pixels (x, y) whose centre is inside a triangle. A pixel with a centre that is not
    clearly in or out (the four points 0.01 pixel off the centre do not agree) is an error."""
    xs = [p[0] for t in shapes for p in t]
    ys = [p[1] for t in shapes for p in t]
    pixels = []
    for y in range(max(0, int(min(ys)) - 1), min(HEIGHT, int(max(ys)) + 2)):
        for x in range(max(0, int(min(xs)) - 1), min(WIDTH, int(max(xs)) + 2)):
            centre = covered(shapes, x + 0.5, y + 0.5)
            for dx, dy in ((-GUARD, -GUARD), (-GUARD, GUARD), (GUARD, -GUARD), (GUARD, GUARD)):
                if covered(shapes, x + 0.5 + dx, y + 0.5 + dy) != centre:
                    raise SystemExit(f"pixel ({x}, {y}) is too close to an edge")
            if centre:
                pixels.append((x, y))
    return pixels


def backdrop() -> rf.Canvas:
    canvas = rf.Canvas(WIDTH, HEIGHT, (0, 0, 0, 255))
    for y, row in enumerate(ta.classroom().rows):
        canvas.pixels[y] = list(zip(row[0::4], row[1::4], row[2::4], row[3::4]))
    return canvas


def arm_frame(shift: float) -> bytes:
    canvas = backdrop()
    for x, y in coverage(triangles(arm_vertices(shift))):
        canvas.pixels[y][x] = ORANGE
    return canvas.png()


def flash_frame() -> bytes:
    """The fade through white at progress 0.25: lerp(old, white, 0.5), as transition_fade does."""
    canvas = backdrop()
    mix = 2 * FLASH_PROGRESS
    canvas.pixels = [[tuple(round((c + (255 - c) * mix)) if i < 3 else c
                            for i, c in enumerate(pixel)) for pixel in row]
                     for row in canvas.pixels]
    return canvas.png()


def goldens() -> dict:
    return {"content_rest.png": arm_frame(0.0),
            "content_slide.png": arm_frame(SLIDE),
            "content_flash.png": flash_frame()}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: content_fixtures.py OUT_DIR", file=sys.stderr)
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
