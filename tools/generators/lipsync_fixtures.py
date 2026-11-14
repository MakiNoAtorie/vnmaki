#!/usr/bin/env python3
"""Generate the lip sync test fixture.

Usage: lipsync_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. It gets voice.wav: 0.2 s of silence, 0.4 s of a 440 Hz tone
(amplitude 0.5, so the RMS level is 0.354), then 0.2 s of silence. Mono, 48 kHz, 16 bit.
"""

import math
import shutil
import struct
import sys
import wave
from pathlib import Path

RATE = 48000
SILENCE_SECONDS = 0.2
TONE_SECONDS = 0.4
HERTZ = 440
AMPLITUDE = 0.5
INT16_MAX = 32767


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: lipsync_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    silence = b"\0\0" * round(RATE * SILENCE_SECONDS)
    tone = b"".join(
        struct.pack("<h", round(AMPLITUDE * INT16_MAX * math.sin(2 * math.pi * HERTZ * i / RATE)))
        for i in range(round(RATE * TONE_SECONDS))
    )
    with wave.open(str(out / "voice.wav"), "wb") as wav:
        wav.setnchannels(1)
        wav.setsampwidth(2)
        wav.setframerate(RATE)
        wav.writeframes(silence + tone + silence)
    return 0


if __name__ == "__main__":
    sys.exit(main())
