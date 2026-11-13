#!/usr/bin/env python3
"""Generate the files of the asset import tests (docs/SPEC.md section 19).

Usage: import_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.

  tone.wav             0.5 s of a 440 Hz sine, amplitude 0.25 (peak -12.04 dBFS), mono, 16 bit
  quiet.wav            the same at 48 kHz stereo and amplitude 0.05, for an RMS gain
  gltf/                glTF files: good.gltf (a triangle with its buffer in a data URI),
                       missing.gltf (a buffer and an image that are not there), short.gltf
                       (its buffer file is shorter than it says), broken.gltf (not JSON),
                       bad_index.gltf (an index that reaches outside of its buffer)
  project/             a project for `vnmaki import`: audio, a font, a shader, a glTF file and the
                       two languages en and ja with a translation that has Japanese letters
  project_broken/      a project whose glTF file has a missing buffer and a missing image

The font is the template's (templates/default/fonts/default.ttf), the shader too: Python cannot
write either.
"""

import base64
import json
import math
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TEMPLATE = ROOT / "templates" / "default"

RATE = 48000
FREQUENCY = 440
SECONDS = 0.5
TONE_AMPLITUDE = 0.25
QUIET_AMPLITUDE = 0.05
PCM_MAX = 32767

PROJECT_TOML = """title = "Import fixture"
languages = ["en", "ja"]

[build.audio]
normalize = "peak"
target_db = -3.0
"""

SCRIPT = """local eileen = character { name = "Eileen" }

label("start", function()
  say(eileen, "Hello.")
  say(eileen, "Good morning \\u{3042}")
end, { ending = true })
"""

JA_PO = """msgid ""
msgstr ""
"Content-Type: text/plain; charset=UTF-8\\n"

msgctxt "start/00000000"
msgid "Hello."
msgstr "こんにちは"
"""


def wav(path: Path, channels: int, amplitude: float) -> None:
    frames = int(RATE * SECONDS)
    data = bytearray()
    for i in range(frames):
        sample = int(amplitude * PCM_MAX * math.sin(2 * math.pi * FREQUENCY * i / RATE))
        data += struct.pack("<h", sample) * channels
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, RATE, RATE * channels * 2,
                                    channels * 2, 16)
    header += b"data" + struct.pack("<I", len(data))
    path.write_bytes(header + bytes(data))


def triangle_buffer() -> bytes:
    positions = [(0.0, 0.0, 0.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)]
    return b"".join(struct.pack("<fff", *p) for p in positions)


def gltf(buffer_uri: str, buffer_length: int, image_uri: str | None = None,
         accessor_count: int = 3) -> str:
    document = {
        "asset": {"version": "2.0"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
        "accessors": [{
            "bufferView": 0, "componentType": 5126, "count": accessor_count, "type": "VEC3",
            "min": [0, 0, 0], "max": [1, 1, 0],
        }],
        "bufferViews": [{"buffer": 0, "byteLength": 36}],
        "buffers": [{"uri": buffer_uri, "byteLength": buffer_length}],
    }
    if image_uri is not None:
        document["images"] = [{"uri": image_uri}]
        document["textures"] = [{"source": 0}]
        document["materials"] = [{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}]
        document["meshes"][0]["primitives"][0]["material"] = 0
    return json.dumps(document, indent=2)


def data_uri(data: bytes) -> str:
    return "data:application/octet-stream;base64," + base64.b64encode(data).decode("ascii")


def write_gltf_files(folder: Path) -> None:
    folder.mkdir(parents=True)
    buffer = triangle_buffer()
    (folder / "good.gltf").write_text(gltf(data_uri(buffer), len(buffer)))
    (folder / "missing.gltf").write_text(gltf("nothing.bin", 36, "nothing.png"))
    (folder / "short.bin").write_bytes(buffer[:8])
    (folder / "short.gltf").write_text(gltf("short.bin", 36))
    (folder / "broken.gltf").write_text("{ this is not JSON")
    # 4 vertices of 12 bytes do not fit in the 36 bytes of the view.
    (folder / "bad_index.gltf").write_text(gltf(data_uri(buffer), len(buffer), accessor_count=4))


def main(out: Path) -> None:
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)
    wav(out / "tone.wav", 1, TONE_AMPLITUDE)
    wav(out / "quiet.wav", 2, QUIET_AMPLITUDE)
    write_gltf_files(out / "gltf")

    project = out / "project"
    for folder in ("scripts", "translations", "audio", "fonts", "shaders", "models"):
        (project / folder).mkdir(parents=True)
    (project / "project.toml").write_text(PROJECT_TOML)
    (project / "scripts" / "main.lua").write_text(SCRIPT)
    (project / "translations" / "ja.po").write_text(JA_PO, encoding="utf-8")
    wav(project / "audio" / "tone.wav", 1, TONE_AMPLITUDE)
    shutil.copy(TEMPLATE / "fonts" / "default.ttf", project / "fonts" / "default.ttf")
    shutil.copy(TEMPLATE / "shaders" / "sepia.frag.hlsl", project / "shaders" / "tint.frag.hlsl")
    shutil.copy(out / "gltf" / "good.gltf", project / "models" / "triangle.gltf")

    broken = out / "project_broken"
    (broken / "models").mkdir(parents=True)
    (broken / "project.toml").write_text('title = "Broken"\n')
    shutil.copy(out / "gltf" / "missing.gltf", broken / "models" / "missing.gltf")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
