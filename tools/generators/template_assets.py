#!/usr/bin/env python3
"""Generate the images, music, model and shader of templates/default/.

Usage: template_assets.py OUT_DIR

OUT_DIR is the template folder (templates/default/), or a copy of the layout. The script
writes only its own files and never deletes anything:

  images/bg/classroom.png            the backdrop, 1280 by 720
  images/eileen/base.png             the layers of the sprite "eileen" (see scripts/main.lua),
  images/eileen/outfit_school.png    all 300 by 560 with a transparent background
  images/eileen/face_smile.png
  images/eileen/face_sad.png
  images/bg/rooftop.png              the backdrop of the presentation demo
  images/eileen/lids_*.png           eyelids (open, half, closed): the frames of the blink
  images/eileen/mouth_*.png          mouths (closed, half, open): the frames of the lip sync
  images/mask/iris.png               a round mask for the "mask" transition
  audio/bgm/theme.wav                a 4 second loop of sine notes
  audio/voice/eileen/hello.wav       a voice line of tone bursts, for lip sync
  audio/sfx/chime.wav                a short chime: the sound with a caption in the content demo
  models/arm.gltf                    a bar with two joints and the clip "bend" (the 3D model of
                                     the content demo; model_fixtures.skinned())
  shaders/sepia.frag.hlsl            a game shader (shader_fixtures.SEPIA_SHADER)

It does not write the fonts: fonts/ and movies/clip.webm are committed files (OFL fonts, see
templates/default/ASSETS.txt). Everything else is made of rectangles, ellipses and sine waves, so
no source art is involved. Those assets are CC0 1.0. Only the standard library is used.
"""

import io
import math
import struct
import sys
import wave
import zlib
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import model_fixtures as mf  # noqa: E402  (the glTF of the 3D model)
import render_fixtures as rf  # noqa: E402  (the PNG chunks)
import shader_fixtures as sf  # noqa: E402  (the sepia shader)

# --- Images ------------------------------------------------------------------------------

BACKDROP_SIZE = (1280, 720)
SPRITE_SIZE = (300, 560)
CLEAR = (0, 0, 0, 0)
IRIS_SIZE = (128, 72)
IRIS_RADIUS = 0.6  # of the width: where the mask is white


class Image:
    """An RGBA image made of rows of bytes, drawn with filled shapes."""

    def __init__(self, width: int, height: int, color: tuple = CLEAR) -> None:
        self.width, self.height = width, height
        self.rows = [bytearray(bytes(color) * width) for _ in range(height)]

    def rect(self, x0: int, y0: int, x1: int, y1: int, color: tuple) -> None:
        """Fills the pixels from (x0, y0) up to, but not including, (x1, y1)."""
        x0, x1 = max(x0, 0), min(x1, self.width)
        for y in range(max(y0, 0), min(y1, self.height)):
            self.rows[y][x0 * 4:x1 * 4] = bytes(color) * max(x1 - x0, 0)

    def ellipse(self, cx: int, cy: int, rx: int, ry: int, color: tuple) -> None:
        for y in range(cy - ry, cy + ry + 1):
            half = round(rx * math.sqrt(max(0.0, 1 - ((y - cy) / ry) ** 2)))
            self.rect(cx - half, y, cx + half + 1, y + 1, color)

    def curve(self, cx: int, cy: int, half_width: int, bend: int, thickness: int,
              color: tuple) -> None:
        """A parabola through (cx - half_width, cy), (cx, cy + bend), (cx + half_width, cy)."""
        for dx in range(-half_width, half_width + 1):
            y = cy + round(bend * (1 - (dx / half_width) ** 2))
            self.rect(cx + dx, y, cx + dx + thickness, y + thickness, color)

    def png(self) -> bytes:
        raw = b"".join(b"\0" + bytes(row) for row in self.rows)
        header = struct.pack(">IIBBBBB", self.width, self.height, 8, rf.COLOR_TYPE_RGBA, 0, 0, 0)
        return (rf.PNG_SIGNATURE + rf.png_chunk(b"IHDR", header) +
                rf.png_chunk(b"IDAT", zlib.compress(raw, 9)) + rf.png_chunk(b"IEND", b""))


def classroom() -> Image:
    wall, floor, plank = (236, 228, 205, 255), (178, 134, 94, 255), (150, 108, 72, 255)
    wood, frame = (120, 84, 56, 255), (92, 64, 44, 255)
    image = Image(*BACKDROP_SIZE, wall)
    width, height = BACKDROP_SIZE
    horizon = 470
    image.rect(0, 440, width, horizon, (210, 198, 170, 255))  # the skirting board
    image.rect(0, horizon, width, height, floor)
    for y in range(horizon + 40, height, 60):
        image.rect(0, y, width, y + 3, plank)
    # The blackboard.
    image.rect(70, 90, 570, 330, frame)
    image.rect(84, 104, 556, 316, (44, 74, 62, 255))
    image.rect(120, 150, 330, 156, (230, 230, 220, 255))
    image.rect(120, 190, 420, 196, (230, 230, 220, 255))
    image.rect(120, 230, 260, 236, (230, 230, 220, 255))
    image.rect(84, 316, 556, 330, wood)
    # A window with a sky and a sun.
    image.rect(800, 70, 1180, 380, frame)
    image.rect(812, 82, 1168, 368, (150, 200, 235, 255))
    image.ellipse(1060, 160, 38, 38, (255, 236, 150, 255))
    image.rect(982, 82, 998, 368, frame)
    image.rect(812, 218, 1168, 232, frame)
    image.rect(790, 380, 1190, 396, wood)
    # A clock.
    image.ellipse(680, 90, 34, 34, frame)
    image.ellipse(680, 90, 28, 28, (250, 248, 240, 255))
    image.rect(678, 66, 682, 92, (40, 40, 40, 255))
    image.rect(680, 88, 700, 92, (40, 40, 40, 255))
    # Two rows of desks.
    for row, (top, size) in enumerate(((520, 150), (590, 190))):
        for left in range(60 + 90 * row, width, 330 - 40 * row):
            image.rect(left, top, left + size, top + 22, wood)
            image.rect(left + 10, top + 22, left + 22, top + 100, frame)
            image.rect(left + size - 22, top + 22, left + size - 10, top + 100, frame)
    return image


SKIN, HAIR, HAIR_SHADE = (250, 221, 200, 255), (200, 162, 200, 255), (168, 128, 170, 255)
BLOUSE, SKIRT, RIBBON = (246, 246, 250, 255), (52, 62, 112, 255), (190, 50, 60, 255)
INK, BLUSH = (60, 40, 70, 255), (244, 170, 170, 255)


def sprite_base() -> Image:
    image = Image(*SPRITE_SIZE)
    image.ellipse(150, 120, 104, 96, HAIR_SHADE)  # the hair behind
    image.rect(46, 120, 254, 330, HAIR_SHADE)
    image.rect(132, 190, 168, 250, SKIN)  # the neck
    image.rect(20, 262, 52, 420, SKIN)  # the arms
    image.rect(248, 262, 280, 420, SKIN)
    image.rect(112, 400, 140, 540, SKIN)  # the legs
    image.rect(160, 400, 188, 540, SKIN)
    image.ellipse(150, 126, 76, 88, SKIN)  # the head
    image.ellipse(150, 66, 80, 46, HAIR)  # the fringe
    image.rect(70, 66, 230, 96, HAIR)
    return image


def sprite_outfit() -> Image:
    image = Image(*SPRITE_SIZE)
    image.rect(70, 244, 230, 350, BLOUSE)
    image.rect(36, 250, 70, 330, BLOUSE)  # the sleeves
    image.rect(230, 250, 264, 330, BLOUSE)
    image.rect(70, 350, 230, 460, SKIRT)
    image.rect(56, 440, 244, 470, SKIRT)
    image.rect(124, 244, 176, 262, RIBBON)  # the ribbon
    image.rect(140, 244, 160, 300, RIBBON)
    return image


def sprite_face(smiling: bool) -> Image:
    image = Image(*SPRITE_SIZE)
    for cx in (112, 188):
        image.ellipse(cx, 134, 11, 16, INK)
        image.rect(cx - 5, 126, cx - 1, 132, BLOUSE)  # a glint
        image.ellipse(cx, 168, 14, 7, BLUSH)
    brow = 0 if smiling else 8  # sad brows slope down at the outer end
    image.rect(96, 104 + brow, 128, 108 + brow, INK)
    image.rect(172, 104 + brow, 204, 108 + brow, INK)
    image.curve(150, 176 if smiling else 192, 22, 12 if smiling else -12, 4, INK)
    return image


def rooftop() -> Image:
    width, height = BACKDROP_SIZE
    bands = ((252, 190, 120), (246, 160, 120), (226, 130, 130), (176, 110, 150), (110, 90, 150))
    image = Image(*BACKDROP_SIZE, bands[0] + (255,))
    band = 200
    for i, (r, g, b) in enumerate(bands):  # the sky, from the horizon up
        image.rect(0, 540 - (i + 1) * band // 2, width, 540 - i * band // 2, (r, g, b, 255))
    image.rect(0, 0, width, 540 - len(bands) * band // 2, bands[-1] + (255,))
    image.ellipse(900, 470, 90, 90, (255, 232, 160, 255))  # the sun
    skyline = (30, 40, 70, 255)
    for left, top, size in ((40, 400, 90), (150, 350, 70), (240, 420, 120), (400, 380, 80),
                            (520, 440, 100), (1000, 360, 110), (1130, 410, 90)):
        image.rect(left, top, left + size, 560, skyline)
    image.rect(0, 560, width, height, (86, 92, 110, 255))  # the roof
    image.rect(0, 560, width, 572, (60, 64, 80, 255))
    for x in range(0, width, 60):  # the fence
        image.rect(x, 440, x + 6, 600, (40, 44, 56, 255))
    image.rect(0, 470, width, 478, (40, 44, 56, 255))
    image.rect(0, 540, width, 548, (40, 44, 56, 255))
    return image


def sprite_lids(amount: int) -> Image:
    """Skin over the eyes of the faces: `amount` is 0 (open), 1 (half) or 2 (closed)."""
    image = Image(*SPRITE_SIZE)
    if amount == 0:
        return image
    bottom, lash = (134, 130) if amount == 1 else (152, 134)
    for cx in (112, 188):
        image.rect(cx - 14, 114, cx + 14, bottom, SKIN)
        image.rect(cx - 12, lash, cx + 12, lash + 4, INK)  # the lashes
    return image


def sprite_mouth(opening: int) -> Image:
    """A mouth over the smile or the frown: `opening` is 0 (closed, nothing), 1 or 2."""
    image = Image(*SPRITE_SIZE)
    if opening == 0:
        return image
    image.rect(126, 176, 178, 200, SKIN)
    image.ellipse(150, 186, 12, 4 + 5 * (opening - 1), INK)
    if opening == 2:
        image.ellipse(150, 190, 7, 3, BLUSH)  # the tongue
    return image


def iris_mask() -> Image:
    """Dark in the middle and light at the edge: the new image shows from the middle first."""
    width, height = IRIS_SIZE
    image = Image(width, height)
    for y in range(height):
        for x in range(width):
            distance = math.hypot((x + 0.5 - width / 2) / width, (y + 0.5 - height / 2) / width)
            level = min(255, round(distance / IRIS_RADIUS * 255))
            image.rows[y][x * 4:x * 4 + 4] = bytes((level, level, level, 255))
    return image


def images() -> dict:
    return {
        "images/bg/classroom.png": classroom().png(),
        "images/eileen/base.png": sprite_base().png(),
        "images/eileen/outfit_school.png": sprite_outfit().png(),
        "images/eileen/face_smile.png": sprite_face(True).png(),
        "images/eileen/face_sad.png": sprite_face(False).png(),
        "images/bg/rooftop.png": rooftop().png(),
        "images/eileen/lids_open.png": sprite_lids(0).png(),
        "images/eileen/lids_half.png": sprite_lids(1).png(),
        "images/eileen/lids_closed.png": sprite_lids(2).png(),
        "images/eileen/mouth_closed.png": sprite_mouth(0).png(),
        "images/eileen/mouth_half.png": sprite_mouth(1).png(),
        "images/eileen/mouth_open.png": sprite_mouth(2).png(),
        "images/mask/iris.png": iris_mask().png(),
    }


# --- Music -------------------------------------------------------------------------------

RATE = 16000
NOTE_SECONDS = 0.5
AMPLITUDE = 0.3
FADE_SECONDS = 0.05
# C major, then A minor: the arpeggio of a slow loop. Hertz.
NOTES = (262, 330, 392, 330, 220, 262, 330, 262)


def wav(frames: bytes) -> bytes:
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(RATE)
        out.writeframes(frames)
    return buffer.getvalue()


def theme() -> bytes:
    frames = bytearray()
    length = round(RATE * NOTE_SECONDS)
    fade = round(RATE * FADE_SECONDS)
    for hertz in NOTES:
        for i in range(length):
            envelope = min(1.0, i / fade, (length - i) / fade)
            sample = AMPLITUDE * envelope * math.sin(2 * math.pi * hertz * i / RATE)
            frames += struct.pack("<h", round(sample * 32767))
    return wav(bytes(frames))


CHIME_HERTZ = 880
CHIME_SECONDS = 0.5


def chime() -> bytes:
    frames = bytearray()
    length = round(RATE * CHIME_SECONDS)
    fade = round(RATE * FADE_SECONDS)
    for i in range(length):
        envelope = min(1.0, i / fade, (length - i) / fade)
        sample = AMPLITUDE * envelope * math.sin(2 * math.pi * CHIME_HERTZ * i / RATE)
        frames += struct.pack("<h", round(sample * 32767))
    return wav(bytes(frames))


# The voice line: (seconds, amplitude) bursts of a tone. The RMS level of a burst is its amplitude
# over the square root of 2, so the mouth of "eileen" (thresholds 0.05 and 0.2 in
# scripts/main.lua) is half open for 0.12 and 0.2, and open for 0.5 and above.
VOICE_HERTZ = 180
VOICE_BURSTS = ((0.15, 0), (0.25, 0.6), (0.1, 0), (0.25, 0.2), (0.1, 0), (0.35, 0.7), (0.1, 0),
                (0.25, 0.12), (0.1, 0), (0.3, 0.5), (0.15, 0))


def voice() -> bytes:
    frames = bytearray()
    fade = round(RATE * FADE_SECONDS)
    for seconds, amplitude in VOICE_BURSTS:
        length = round(RATE * seconds)
        for i in range(length):
            envelope = min(1.0, i / fade, (length - i) / fade)
            sample = amplitude * envelope * math.sin(2 * math.pi * VOICE_HERTZ * i / RATE)
            frames += struct.pack("<h", round(sample * 32767))
    return wav(bytes(frames))


def files() -> dict:
    return {**images(), "audio/bgm/theme.wav": theme(), "audio/voice/eileen/hello.wav": voice(),
            "audio/sfx/chime.wav": chime(), "models/arm.gltf": mf.text(mf.skinned()),
            "shaders/sepia.frag.hlsl": sf.SEPIA_SHADER.encode()}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: template_assets.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    for name, content in files().items():
        path = out / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
