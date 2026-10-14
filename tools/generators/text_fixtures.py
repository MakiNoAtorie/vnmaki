#!/usr/bin/env python3
"""Generate the font the text tests use.

Usage: text_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Layout:

  OUT_DIR/fonts/boxes.ttf          a TrueType font whose glyphs are plain rectangles
  OUT_DIR/fonts/boxes_bold.ttf     the same, with Latin letters 600 units wide (bold stand-in)
  OUT_DIR/fonts/boxes_italic.ttf   the same, with Latin letters 400 units wide (italic stand-in)
  OUT_DIR/fonts/fallback.ttf       only the Greek letters, which boxes.ttf lacks, 750 units wide
  OUT_DIR/fonts/*.ttf              broken fonts the loader must reject

Every glyph of boxes.ttf is one axis-aligned box, so tests can state glyph
positions and pixels as literals. 1000 units per em, ascent 800, descent 200,
no line gap: at 20 pixels per em a line is 20 pixels high with its baseline 16
pixels down, and 50 font units are one pixel. Glyph 0 is .notdef; the mapped
characters follow in code point order, so the glyph of the n-th lowest code
point is n (space is glyph 1). The Hebrew letters are there for right-to-left
text.

tools/generators/render_fixtures.py imports glyph_box() to draw the golden
image of the text scene.
"""

import shutil
import struct
import sys
from pathlib import Path

UNITS_PER_EM = 1000
ASCENT = 800
DESCENT = -200

# Glyph shapes: (advance, box) with box = (x_min, y_min, x_max, y_max) in font
# units, y up from the baseline; None is a glyph without an outline.
BLANK = (500, None)
LOWER = (500, (50, 0, 450, 500))
UPPER = (500, (50, 0, 450, 700))
DOT = (500, (150, 0, 350, 200))
WIDE = (1000, (50, -100, 950, 800))
WIDE_DOT = (1000, (100, 0, 400, 300))
BOLD = (600, (50, 0, 550, 700))
ITALIC = (400, (50, 0, 350, 700))
GREEK = (750, (50, 0, 700, 600))


def character_shapes() -> dict:
    shapes = {" ": BLANK}
    for first, last, shape in (("a", "z", LOWER), ("A", "Z", UPPER), ("0", "9", UPPER),
                               ("\u05d0", "\u05ea", LOWER),  # Hebrew, right to left
                               ("ぁ", "ゖ", WIDE),    # hiragana
                               ("ァ", "ヺ", WIDE)):   # katakana
        for code in range(ord(first), ord(last) + 1):
            shapes[chr(code)] = shape
    for char in ".,":
        shapes[char] = DOT
    for char in "!?-":
        shapes[char] = UPPER
    for char in "世界日本語ー「」":  # 世界日本語ー「」
        shapes[char] = WIDE
    for char in "。、":  # 。、
        shapes[char] = WIDE_DOT
    return shapes


def restyled(shapes: dict, latin: tuple) -> dict:
    """`shapes` with every ASCII letter and number drawn as `latin`."""
    return {char: latin if char.isascii() and char.isalnum() else shape
            for char, shape in shapes.items()}


SHAPES = character_shapes()
FALLBACK_SHAPES = {chr(code): GREEK for code in range(ord("\u03b1"), ord("\u03c9") + 1)}


def glyph_list(shapes: dict) -> list:
    # Glyph order: .notdef, then the characters by code point.
    return [UPPER] + [shapes[char] for char in sorted(shapes)]


def glyph_box(char: str) -> tuple:
    """(advance, box) of the glyph for `char`; see the shapes above."""
    return SHAPES[char]


# --- TrueType ----------------------------------------------------------------

def pad4(data: bytes) -> bytes:
    return data + b"\0" * (-len(data) % 4)


def checksum(data: bytes) -> int:
    data = pad4(data)
    return sum(struct.unpack(f">{len(data) // 4}I", data)) & 0xFFFFFFFF


def glyf_and_loca(glyphs: list) -> tuple:
    glyf = b""
    offsets = [0]
    for _, box in glyphs:
        if box:
            x0, y0, x1, y1 = box
            # One contour of four on-curve points, clockwise from the bottom-left corner, as
            # 16-bit coordinate deltas; no instructions.
            glyf += struct.pack(">hhhhh", 1, x0, y0, x1, y1)
            glyf += struct.pack(">HH4B", 3, 0, 1, 1, 1, 1)
            glyf += struct.pack(">4h", x0, 0, x1 - x0, 0)
            glyf += struct.pack(">4h", y0, y1 - y0, 0, y0 - y1)
            glyf = pad4(glyf)
        offsets.append(len(glyf))
    return glyf, struct.pack(f">{len(offsets)}I", *offsets)


def cmap(shapes: dict) -> bytes:
    # One format 12 subtable (Windows, full Unicode) with one group per character.
    groups = b"".join(struct.pack(">III", ord(char), ord(char), index + 1)
                      for index, char in enumerate(sorted(shapes)))
    subtable = struct.pack(">HHIII", 12, 0, 16 + len(groups), 0, len(shapes)) + groups
    return struct.pack(">HHHHI", 0, 1, 3, 10, 12) + subtable


def build_font(shapes: dict) -> bytes:
    glyphs = glyph_list(shapes)
    boxes = [box for _, box in glyphs if box]
    x_min, y_min = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x_max, y_max = max(b[2] for b in boxes), max(b[3] for b in boxes)
    advance_max = max(advance for advance, _ in glyphs)
    glyf, loca = glyf_and_loca(glyphs)
    tables = {
        b"cmap": cmap(shapes),
        b"glyf": glyf,
        # checkSumAdjustment (offset 8) is filled in below; long loca offsets.
        b"head": struct.pack(">IIIIHHqqhhhhHHhhh", 0x00010000, 0x00010000, 0, 0x5F0F3CF5, 0,
                             UNITS_PER_EM, 0, 0, x_min, y_min, x_max, y_max, 0, 8, 2, 1, 0),
        b"hhea": struct.pack(">IhhhHhhhhhh4hhH", 0x00010000, ASCENT, DESCENT, 0, advance_max,
                             x_min, 0, x_max, 1, 0, 0, 0, 0, 0, 0, 0, len(glyphs)),
        b"hmtx": b"".join(struct.pack(">Hh", advance, box[0] if box else 0)
                          for advance, box in glyphs),
        b"loca": loca,
        b"maxp": struct.pack(">IH13H", 0x00010000, len(glyphs), 4, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0,
                             0, 0),
        b"post": struct.pack(">IIhhIIIII", 0x00030000, 0, -100, 50, 0, 0, 0, 0, 0),
    }
    count = len(tables)
    entry_selector = count.bit_length() - 1
    search_range = 16 << entry_selector
    header = struct.pack(">IHHHH", 0x00010000, count, search_range, entry_selector,
                         count * 16 - search_range)
    offset = len(header) + 16 * count
    directory = b""
    body = b""
    head_offset = 0
    for tag in sorted(tables):
        data = tables[tag]
        if tag == b"head":
            head_offset = offset
        directory += struct.pack(">4sIII", tag, checksum(data), offset, len(data))
        body += pad4(data)
        offset += len(pad4(data))
    font = bytearray(header + directory + body)
    adjustment = (0xB1B0AFBA - checksum(bytes(font))) & 0xFFFFFFFF
    font[head_offset + 8:head_offset + 12] = struct.pack(">I", adjustment)
    return bytes(font)


def font_fixtures() -> dict:
    font = build_font(SHAPES)
    return {
        "boxes.ttf": font,
        "boxes_bold.ttf": build_font(restyled(SHAPES, BOLD)),
        "boxes_italic.ttf": build_font(restyled(SHAPES, ITALIC)),
        "fallback.ttf": build_font(FALLBACK_SHAPES),
        "not_a_font.ttf": b"this is plain text, not a font\n",
        "truncated.ttf": font[:len(font) // 2],
        "empty.ttf": b"",
    }


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: text_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    (out / "fonts").mkdir(parents=True)
    for name, content in font_fixtures().items():
        (out / "fonts" / name).write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
