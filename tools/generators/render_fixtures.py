#!/usr/bin/env python3
"""Generate the images the image-loader and renderer tests use.

Usage: render_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Layout:

  OUT_DIR/images/   files for the image decoder (PNG, JPEG, broken files) and
                    textures for the renderer tests
  OUT_DIR/golden/   what each renderer test scene must look like, computed by
                    the reference rasterizer below

Pixel values are spelled out here and repeated as literals in the tests
(tests/unit/image_test.cpp, tests/render/renderer_test.cpp), which are the
specification they check.
"""

import shutil
import struct
import sys
import zlib
from pathlib import Path

# --- PNG -------------------------------------------------------------------

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
COLOR_TYPE_RGB = 2
COLOR_TYPE_RGBA = 6


def png_chunk(kind: bytes, data: bytes) -> bytes:
    crc = zlib.crc32(kind + data) & 0xFFFFFFFF
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", crc)


def encode_png(width: int, height: int, rows: list, color_type: int) -> bytes:
    """rows: list of rows, each a list of pixel tuples (RGB or RGBA, 0-255)."""
    raw = bytearray()
    for row in rows:
        raw.append(0)  # filter type: None
        for pixel in row:
            raw.extend(pixel)
    header = struct.pack(">IIBBBBB", width, height, 8, color_type, 0, 0, 0)
    return (PNG_SIGNATURE + png_chunk(b"IHDR", header) +
            png_chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + png_chunk(b"IEND", b""))


def png_rgba(pixels: list) -> bytes:
    return encode_png(len(pixels[0]), len(pixels), pixels, COLOR_TYPE_RGBA)


def with_empty_idat(png: bytes) -> bytes:
    """The same PNG with a zero-length IDAT chunk before its real one (valid PNG)."""
    first_idat = png.index(b"IDAT") - 4
    return png[:first_idat] + png_chunk(b"IDAT", b"") + png[first_idat:]


# --- JPEG ------------------------------------------------------------------
# A minimal baseline JPEG encoder for images made of flat-coloured 8x8 blocks:
# with every quantizer set to 1 and no AC energy, each block is just its DC
# coefficient, so no DCT is needed. Colour is stored as JFIF YCbCr without
# subsampling.

def ycbcr(rgb: tuple) -> tuple:
    r, g, b = rgb
    y = 0.299 * r + 0.587 * g + 0.114 * b
    cb = -0.168736 * r - 0.331264 * g + 0.5 * b + 128
    cr = 0.5 * r - 0.418688 * g - 0.081312 * b + 128
    return tuple(max(0, min(255, round(v))) for v in (y, cb, cr))


class BitWriter:
    def __init__(self) -> None:
        self.out = bytearray()
        self.acc = 0
        self.count = 0

    def write(self, value: int, length: int) -> None:
        for shift in range(length - 1, -1, -1):
            self.acc = (self.acc << 1) | ((value >> shift) & 1)
            self.count += 1
            if self.count == 8:
                self.out.append(self.acc)
                if self.acc == 0xFF:
                    self.out.append(0)  # byte stuffing
                self.acc = 0
                self.count = 0

    def flush(self) -> bytes:
        while self.count != 0:
            self.write(1, 1)  # pad with one-bits
        return bytes(self.out)


def encode_jpeg_blocks(blocks_wide: int, blocks_high: int, colors: list) -> bytes:
    """colors: one RGB tuple per 8x8 block, row by row."""
    width, height = blocks_wide * 8, blocks_high * 8
    # DC table: categories 0..11, every code 4 bits long (0000 .. 1011).
    dc_lengths = [0, 0, 0, 12] + [0] * 12
    dc_symbols = list(range(12))
    # AC table: only end-of-block (symbol 0x00), code "0".
    ac_lengths = [1] + [0] * 15
    ac_symbols = [0]

    def dht(table_class: int, table_id: int, lengths: list, symbols: list) -> bytes:
        body = bytes([table_class << 4 | table_id]) + bytes(lengths) + bytes(symbols)
        return b"\xff\xc4" + struct.pack(">H", len(body) + 2) + body

    def segment(marker: bytes, body: bytes) -> bytes:
        return marker + struct.pack(">H", len(body) + 2) + body

    dqt = segment(b"\xff\xdb", bytes([0]) + bytes([1] * 64))
    sof = segment(b"\xff\xc0", struct.pack(">BHHB", 8, height, width, 3) +
                  bytes([1, 0x11, 0, 2, 0x11, 0, 3, 0x11, 0]))
    sos = segment(b"\xff\xda", bytes([3, 1, 0x00, 2, 0x00, 3, 0x00, 0, 63, 0]))

    bits = BitWriter()
    previous = [0, 0, 0]
    for rgb in colors:
        for component, value in enumerate(ycbcr(rgb)):
            dc = 8 * (value - 128)  # the DC coefficient of a flat block when Q = 1
            diff = dc - previous[component]
            previous[component] = dc
            category = abs(diff).bit_length()
            bits.write(category, 4)
            if category:
                bits.write(diff if diff > 0 else diff + (1 << category) - 1, category)
            bits.write(0, 1)  # end of block
    return (b"\xff\xd8" + dqt + sof + dht(0, 0, dc_lengths, dc_symbols) +
            dht(1, 0, ac_lengths, ac_symbols) + sos + bits.flush() + b"\xff\xd9")


# --- Golden scenes -------------------------------------------------------------
# A reference rasterizer for what tests/render/renderer_test.cpp draws: axis-
# aligned quads on whole pixels, nearest-texel sampling at pixel centres, and
# premultiplied-alpha blending into an 8-bit target (rounded after every draw,
# as the GPU stores it). Each scene below is drawn the same way by the test.

TARGET_SIZE = 32

# 4x4 texture, every texel a different opaque colour.
CHECKER = [[(x * 80, y * 80, 255 - (x + y) * 30, 255) for x in range(4)] for y in range(4)]
WHITE = [[(255, 255, 255, 255)]]
GRAY = (26, 26, 26, 255)

LUMA = (0.299, 0.587, 0.114)


class Canvas:
    def __init__(self, width: int, height: int, clear: tuple) -> None:
        self.width = width
        self.height = height
        self.pixels = [[premultiply(clear)] * width for _ in range(height)]

    def draw(self, texture: list, dest: tuple, uv: tuple = (0, 0, 1, 1),
             tint: tuple = (255, 255, 255, 255), blend: str = "alpha",
             shade=None) -> None:
        """dest: (x, y, w, h) in whole pixels; uv: (u, v, w, h); tint: straight RGBA8."""
        x0, y0, w, h = dest
        tex_h, tex_w = len(texture), len(texture[0])
        tint_alpha = tint[3] / 255
        tint_pm = [tint[0] / 255 * tint_alpha, tint[1] / 255 * tint_alpha,
                   tint[2] / 255 * tint_alpha, tint_alpha]
        for py in range(max(0, y0), min(self.height, y0 + h)):
            for px in range(max(0, x0), min(self.width, x0 + w)):
                u = uv[0] + (px + 0.5 - x0) / w * uv[2]
                v = uv[1] + (py + 0.5 - y0) / h * uv[3]
                tx = min(tex_w - 1, max(0, int(u * tex_w)))
                ty = min(tex_h - 1, max(0, int(v * tex_h)))
                texel = [c / 255 for c in texture[ty][tx]]  # premultiplied already
                src = [texel[i] * tint_pm[i] for i in range(4)]
                if shade:
                    src = shade(src)
                dst = [c / 255 for c in self.pixels[py][px]]
                if blend == "alpha":
                    out = [src[i] + dst[i] * (1 - src[3]) for i in range(4)]
                elif blend == "additive":
                    out = [min(1.0, src[i] + dst[i]) for i in range(4)]
                else:  # replace
                    out = src
                self.pixels[py][px] = tuple(round(c * 255) for c in out)

    def png(self) -> bytes:
        # Golden scenes end opaque, so premultiplied and straight alpha agree,
        # except where a scene writes transparent black (also the same in both).
        return png_rgba(self.pixels)


def premultiply(rgba: tuple) -> tuple:
    r, g, b, a = rgba
    return (round(r * a / 255), round(g * a / 255), round(b * a / 255), a)


def as_texture(canvas: Canvas) -> list:
    return canvas.pixels


def grayscale(amount: float):
    """Reference for tests/render/shaders/grayscale.frag.hlsl."""
    def shade(src: list) -> list:
        gray = sum(src[i] * LUMA[i] for i in range(3))
        return [src[i] + (gray - src[i]) * amount for i in range(3)] + [src[3]]
    return shade


def golden_scenes() -> dict:
    scenes = {}

    canvas = Canvas(TARGET_SIZE, TARGET_SIZE, GRAY)
    canvas.draw(CHECKER, (4, 4, 16, 16))
    canvas.draw(CHECKER, (22, 4, 8, 8), uv=(0.5, 0, 0.5, 0.5))
    canvas.draw(CHECKER, (4, 22, 8, 8), tint=(255, 128, 64, 255))
    scenes["textured_quad"] = canvas

    canvas = Canvas(TARGET_SIZE, TARGET_SIZE, (255, 255, 255, 255))
    canvas.draw(WHITE, (4, 4, 16, 16), tint=(255, 0, 0, 128))
    canvas.draw(WHITE, (12, 12, 16, 16), tint=(0, 0, 255, 64))
    canvas.draw(WHITE, (6, 6, 4, 4), tint=(0, 64, 0, 255), blend="additive")
    canvas.draw(WHITE, (24, 2, 6, 6), tint=(0, 0, 0, 0), blend="replace")
    scenes["alpha_blend"] = canvas

    layer = Canvas(8, 8, (0, 0, 0, 0))
    layer.draw(CHECKER, (2, 2, 4, 4))
    canvas = Canvas(TARGET_SIZE, TARGET_SIZE, GRAY)
    canvas.draw(as_texture(layer), (8, 8, 16, 16))
    scenes["render_target"] = canvas

    canvas = Canvas(TARGET_SIZE, TARGET_SIZE, GRAY)
    canvas.draw(CHECKER, (4, 4, 8, 8), shade=grayscale(1.0))
    canvas.draw(CHECKER, (20, 4, 8, 8), shade=grayscale(0.0))
    canvas.draw(CHECKER, (4, 20, 8, 8), shade=grayscale(0.5))
    scenes["custom_shader"] = canvas

    canvas = Canvas(TARGET_SIZE, TARGET_SIZE, (0, 0, 0, 255))
    for y in range(4):
        for x in range(4):
            canvas.draw(CHECKER, (x * 8 + 1, y * 8 + 1, 6, 6), uv=(x / 4, y / 4, 0.25, 0.25))
    scenes["sprite_batch"] = canvas

    return {name + ".png": canvas.png() for name, canvas in scenes.items()}


# --- Fixtures ----------------------------------------------------------------

def image_fixtures() -> dict:
    files = {}
    # Row 0: opaque red, opaque green. Row 1: half-transparent blue, fully
    # transparent white.
    files["rgba_2x2.png"] = png_rgba([
        [(255, 0, 0, 255), (0, 255, 0, 255)],
        [(0, 0, 255, 128), (255, 255, 255, 0)],
    ])
    # No alpha channel: every pixel is opaque.
    files["rgb_3x1.png"] = encode_png(3, 1, [[(10, 20, 30), (40, 50, 60), (70, 80, 90)]],
                                      COLOR_TYPE_RGB)
    files["empty_idat_rgb_3x1.png"] = with_empty_idat(files["rgb_3x1.png"])
    # Two flat 8x8 blocks side by side: orange on the left, teal on the right.
    files["blocks_16x8.jpg"] = encode_jpeg_blocks(2, 1, [(240, 120, 32), (32, 160, 160)])
    files["not_an_image.png"] = b"this is plain text, not a PNG\n"
    valid = png_rgba([[(1, 2, 3, 4)] * 8] * 8)
    files["truncated.png"] = valid[: len(valid) // 2]
    files["empty.png"] = b""
    # A valid PNG wider than the decoder allows (16384 pixels).
    files["too_wide.png"] = encode_png(16385, 1, [[(0, 0, 0, 0)] * 16385], COLOR_TYPE_RGBA)
    # A header claiming 8193x8193 pixels, over the 2^26-pixel budget, with no
    # pixel data: the decoder must refuse it from the header alone.
    header = struct.pack(">IIBBBBB", 8193, 8193, 8, COLOR_TYPE_RGBA, 0, 0, 0)
    files["too_many_pixels.png"] = (PNG_SIGNATURE + png_chunk(b"IHDR", header) +
                                    png_chunk(b"IEND", b""))
    # Textures the golden scenes draw.
    files["checker_4x4.png"] = png_rgba(CHECKER)
    files["white_1x1.png"] = png_rgba(WHITE)
    return files


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: render_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for directory, files in (("images", image_fixtures()), ("golden", golden_scenes())):
        for name, content in files.items():
            path = out / directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
