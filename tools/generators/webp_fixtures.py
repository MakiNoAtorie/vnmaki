#!/usr/bin/env python3
"""Generate the WebP images the image-decoder test uses.

Usage: webp_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Layout:

  OUT_DIR/images/   rgba_2x2.webp (lossless), truncated.webp (broken)
  OUT_DIR/seeds/    rgba_2x2.vp8l: the VP8L chunk data of rgba_2x2.webp, a seed for
                    fuzz/webp_fuzzer.cpp

The writer makes the smallest valid lossless WebP (VP8L): no transforms, no
colour cache, no backward references, and one prefix code per channel. Each
channel has at most two values, so each code is a "simple" code of one bit per
pixel. The pixel values are repeated as literals in tests/unit/image_test.cpp,
which is the specification they check.
"""

import shutil
import struct
import sys
from pathlib import Path

VP8L_SIGNATURE = 0x2F
SIMPLE_CODE = 1
# Order of the channels in the bit stream: green, red, blue, alpha.
CHANNEL_ORDER = (1, 0, 2, 3)
BYTE_BITS = 8
FIRST_SYMBOL_IS_8_BITS = 1

# (R, G, B, A) in rows from top to bottom.
PIXELS_2X2 = [
    [(255, 0, 0, 255), (0, 255, 0, 255)],
    [(0, 0, 255, 255), (255, 255, 255, 128)],
]


class BitWriter:
    """Writes bits starting at the least significant bit of each byte."""

    def __init__(self) -> None:
        self.value = 0
        self.count = 0

    def write(self, value: int, bits: int) -> None:
        self.value |= value << self.count
        self.count += bits

    def to_bytes(self) -> bytes:
        return self.value.to_bytes((self.count + 7) // 8, "little")


def encode_vp8l(pixels: list) -> bytes:
    """pixels: rows of (R, G, B, A) tuples. Each channel has one or two values."""
    height, width = len(pixels), len(pixels[0])
    flat = [p for row in pixels for p in row]
    bits = BitWriter()
    bits.write(width - 1, 14)
    bits.write(height - 1, 14)
    bits.write(1 if any(p[3] != 255 for p in flat) else 0, 1)  # alpha is used
    bits.write(0, 3)  # version
    bits.write(0, 1)  # no transform
    bits.write(0, 1)  # no colour cache
    bits.write(0, 1)  # no meta prefix image
    codes = {}
    for channel in CHANNEL_ORDER:
        symbols = sorted({p[channel] for p in flat})
        assert len(symbols) <= 2, "a simple code holds at most two symbols"
        bits.write(SIMPLE_CODE, 1)
        bits.write(len(symbols) - 1, 1)
        bits.write(FIRST_SYMBOL_IS_8_BITS, 1)
        bits.write(symbols[0], BYTE_BITS)
        if len(symbols) == 2:
            bits.write(symbols[1], BYTE_BITS)
        codes[channel] = symbols
    # The distance code: simple, one symbol, a 1-bit symbol with the value 0.
    bits.write(SIMPLE_CODE, 1)
    bits.write(0, 3)
    for pixel in flat:
        for channel in CHANNEL_ORDER:
            symbols = codes[channel]
            if len(symbols) == 2:  # the smaller symbol has the code 0
                bits.write(symbols.index(pixel[channel]), 1)
    return bytes([VP8L_SIGNATURE]) + bits.to_bytes()


def riff_webp(vp8l: bytes) -> bytes:
    padded = vp8l + (b"\0" if len(vp8l) % 2 else b"")
    body = b"WEBP" + b"VP8L" + struct.pack("<I", len(vp8l)) + padded
    return b"RIFF" + struct.pack("<I", len(body)) + body


def main() -> None:
    out = Path(sys.argv[1])
    if out.exists():
        shutil.rmtree(out)
    images = out / "images"
    images.mkdir(parents=True)
    vp8l = encode_vp8l(PIXELS_2X2)
    webp = riff_webp(vp8l)
    (out / "seeds").mkdir()
    (out / "seeds" / "rgba_2x2.vp8l").write_bytes(vp8l)
    (images / "rgba_2x2.webp").write_bytes(webp)
    (images / "truncated.webp").write_bytes(webp[:-3])


if __name__ == "__main__":
    main()
