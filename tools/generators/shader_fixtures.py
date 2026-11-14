#!/usr/bin/env python3
"""Generate the fixtures of the game shader GPU test (tests/render/shader_render_test.cpp).

Usage: shader_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/game/    a game project: a sepia shader (shaders/sepia.frag.hlsl) and a script that
                   puts it on a sprite (label start) or on a layer (label layer), and then
                   tweens its uniform
  OUT_DIR/golden/  what the frames must look like, drawn by the reference rasterizer of
                   render_fixtures.py
  OUT_DIR/broken/  a game whose shader has a compile error on line 5

Each label shows the backdrop, a sprite at the bottom centre of the 32x32 scene and a second
sprite on the overlay layer. The uniform "amount" is 0.25, and half a second into a tween to 1
over one second it is 0.625.
"""

import shutil
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the reference rasterizer and the PNG writer)

SEPIA_TONE = (1.0, 0.8, 0.6)
AMOUNT_LOW = 0.25
AMOUNT_HIGH = 0.625

SEPIA_SHADER = """\
#include "vnmaki.hlsli"

cbuffer SepiaUniforms : register(b0, space3) {
  float amount;
  float3 unused_padding;
};

static const float3 kLuma = float3(0.299, 0.587, 0.114);
static const float3 kTone = float3(1.0, 0.8, 0.6);

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 color = sample_sprite(input) * input.color;
  const float3 toned = dot(color.rgb, kLuma) * kTone;
  return float4(lerp(color.rgb, toned, amount), color.a);
}
"""

BROKEN_SHADER = """\
#include "vnmaki.hlsli"

float4 main(SpriteFragment input) : SV_Target0 {
  const float4 color = sample_sprite(input);
  return color * missing_name;
}
"""

SCRIPT = """\
shader("sepia", { "amount" })

label("start", function()
  scene("bg/room")
  show("eileen", { shader = "sepia", uniforms = { amount = %s } })
  show("lucy", { image = "eileen", x = 0.25, y = 0.5, layer = "overlay" })
  narrate("shaded sprite")
  tween("eileen", { uniforms = { amount = 1 }, duration = 1 })
  narrate("tweening")
end)

label("layer", function()
  scene("bg/room")
  show("eileen")
  show("lucy", { image = "eileen", x = 0.25, y = 0.5, layer = "overlay" })
  layer("overlay", { shader = "sepia", uniforms = { amount = %s } })
  narrate("shaded layer")
  tween("@layer.overlay", { uniforms = { amount = 1 }, duration = 1 })
  narrate("tweening")
end)
""" % (AMOUNT_LOW, AMOUNT_LOW)

BROKEN_SCRIPT = """\
shader("broken")

label("start", function()
  narrate("never reached")
end)
"""


def sepia(amount: float):
    """Reference for the sepia shader: the tinted texel, a little toward sepia."""
    def shade(src: list) -> list:
        gray = sum(src[i] * rf.LUMA[i] for i in range(3))
        return [src[i] + (gray * SEPIA_TONE[i] - src[i]) * amount for i in range(3)] + [src[3]]
    return shade


def game() -> dict:
    return {
        "scripts/start.lua": SCRIPT.encode("utf-8"),
        "images/bg/room.png": rf.png_rgba(rf.BACKDROP),
        "images/eileen.png": rf.png_rgba(rf.SPRITE),
        "shaders/sepia.frag.hlsl": SEPIA_SHADER.encode("utf-8"),
    }


def broken() -> dict:
    return {
        "scripts/start.lua": BROKEN_SCRIPT.encode("utf-8"),
        "shaders/broken.frag.hlsl": BROKEN_SHADER.encode("utf-8"),
    }


def goldens() -> dict:
    sprite = [[rf.premultiply(pixel) for pixel in row] for row in rf.SPRITE]
    center = (12, 16, rf.SPRITE_WIDTH, rf.SPRITE_HEIGHT)
    overlay = (4, 0, rf.SPRITE_WIDTH, rf.SPRITE_HEIGHT)
    size = rf.TARGET_SIZE
    scenes = {}
    for name, amount in (("low", AMOUNT_LOW), ("high", AMOUNT_HIGH)):
        # The shader is on the sprite.
        canvas = rf.Canvas(size, size, (0, 0, 0, 255))
        canvas.draw(rf.BACKDROP, (0, 0, size, size))
        canvas.draw(sprite, center, shade=sepia(amount))
        canvas.draw(sprite, overlay)
        scenes["sepia_sprite_" + name] = canvas

        # The shader is on the overlay layer, which is drawn alone first.
        layer = rf.Canvas(size, size, (0, 0, 0, 0))
        layer.draw(sprite, overlay)
        canvas = rf.Canvas(size, size, (0, 0, 0, 255))
        canvas.draw(rf.BACKDROP, (0, 0, size, size))
        canvas.draw(sprite, center)
        canvas.draw(layer.pixels, (0, 0, size, size), shade=sepia(amount))
        scenes["sepia_layer_" + name] = canvas
    return {name + ".png": canvas.png() for name, canvas in scenes.items()}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: shader_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for directory, files in (("game", game()), ("golden", goldens()), ("broken", broken())):
        for name, content in files.items():
            path = out / directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
