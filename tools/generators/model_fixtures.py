#!/usr/bin/env python3
"""Generate the fixtures of the 3D tests (tests/unit/model3d_test.cpp and
tests/render/model3d_render_test.cpp).

Usage: model_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated. Layout:

  OUT_DIR/game/    a game project with a 64x64 virtual screen: models/ (glTF cubes written with
                   json and struct only), shaders/whiten.frag.hlsl and scripts/start.lua, one
                   label for each GPU case
  OUT_DIR/golden/  unlit.png, drawn by the reference below

The cube has half-size 1, 24 vertices (4 per face, with normals and texture coordinates) and 36
16-bit indices. Models:

  cube.gltf    orange, buffer in a data URI          blue.gltf    blue
  cube.glb     orange, buffer in the file            rough.gltf   orange, metallic 0, roughness 1
  tex.gltf     white, external tex.bin and tex.png   metal.gltf   orange, metallic 1, roughness 0.2
  tex.glb      white, the texture inside the file    moved.gltf   a node with a translation and scale
  bad/         files that must fail to load

The reference of the unlit case does not need a triangle rasterizer. The camera looks along -z at
two cubes centred on the axis, so only the front face of each shows, as a square. With a vertical
field of view of 90 degrees (tan 45 = 1) a point at distance d and offset x lands x / d of half
the screen from the centre. The camera is at z = 5: the near cube (half-size 1, front face at
z = 1, d = 4) covers half-width 1/4 of 32 = 8 pixels, and the far cube (centre z = -4, half-size
3, front face at z = -1, d = 6) covers 3/6 of 32 = 16 pixels. Every edge falls on a whole pixel.
"""

import base64
import json
import shutil
import struct
import sys
from pathlib import Path

sys.dont_write_bytecode = True  # No __pycache__ in the source tree.
import render_fixtures as rf  # noqa: E402  (the PNG writer and the Canvas)

SIZE = 64
ORANGE = (0.8, 0.4, 0.2)
BLUE = (0.2, 0.4, 0.8)
WHITE = (1.0, 1.0, 1.0)

# Each face: the normal and its four corners, counter-clockwise seen from outside.
FACES = [
    ((0, 0, 1), [(-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)]),
    ((0, 0, -1), [(1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)]),
    ((1, 0, 0), [(1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)]),
    ((-1, 0, 0), [(-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)]),
    ((0, 1, 0), [(-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)]),
    ((0, -1, 0), [(-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1)]),
]
UVS = [(0, 1), (1, 1), (1, 0), (0, 0)]  # v = 0 is the top of the picture

# The texture: red, green / blue, white (top row first).
TEXTURE = [[(255, 0, 0, 255), (0, 255, 0, 255)], [(0, 0, 255, 255), (255, 255, 255, 255)]]

ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
FLOAT = 5126
UNSIGNED_SHORT = 5123


def geometry() -> dict:
    positions, normals, uvs, indices = [], [], [], []
    for normal, corners in FACES:
        base = len(positions)
        for corner, uv in zip(corners, UVS):
            positions.append(corner)
            normals.append(normal)
            uvs.append(uv)
        indices += [base, base + 1, base + 2, base, base + 2, base + 3]
    return {"positions": positions, "normals": normals, "uvs": uvs, "indices": indices}


def pack_buffer(index_override=None) -> tuple:
    """The binary buffer and the byte length of each of its four parts."""
    g = geometry()
    indices = list(g["indices"])
    if index_override is not None:
        indices[0] = index_override
    parts = [
        b"".join(struct.pack("<3f", *p) for p in g["positions"]),
        b"".join(struct.pack("<3f", *n) for n in g["normals"]),
        b"".join(struct.pack("<2f", *uv) for uv in g["uvs"]),
        b"".join(struct.pack("<H", i) for i in indices),
    ]
    return b"".join(parts), [len(part) for part in parts]


def gltf_json(*, color=ORANGE, metallic=0.0, roughness=0.5, textured=False, buffer_uri=None,
              image=None, node=None, mode=4, index_override=None) -> tuple:
    """The glTF document and the binary buffer. `image` is {"uri": ...} or {"bufferView": n}."""
    data, lengths = pack_buffer(index_override)
    offsets = [sum(lengths[:i]) for i in range(len(lengths))]
    views = [{"buffer": 0, "byteOffset": o, "byteLength": n, "target": t}
             for o, n, t in zip(offsets, lengths, [ARRAY_BUFFER] * 3 + [ELEMENT_ARRAY_BUFFER])]
    count = len(geometry()["positions"])
    accessors = [
        {"bufferView": 0, "componentType": FLOAT, "count": count, "type": "VEC3",
         "min": [-1, -1, -1], "max": [1, 1, 1]},
        {"bufferView": 1, "componentType": FLOAT, "count": count, "type": "VEC3"},
        {"bufferView": 2, "componentType": FLOAT, "count": count, "type": "VEC2"},
        {"bufferView": 3, "componentType": UNSIGNED_SHORT, "count": len(geometry()["indices"]),
         "type": "SCALAR"},
    ]
    pbr = {"baseColorFactor": [*color, 1.0], "metallicFactor": metallic,
           "roughnessFactor": roughness}
    document = {
        "asset": {"version": "2.0", "generator": "model_fixtures.py"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [node or {"mesh": 0}],
        "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2},
                                    "indices": 3, "material": 0, "mode": mode}]}],
        "materials": [{"pbrMetallicRoughness": pbr}],
        "buffers": [{"byteLength": len(data)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    if buffer_uri is not None:
        document["buffers"][0]["uri"] = buffer_uri
    if textured:
        pbr["baseColorTexture"] = {"index": 0}
        document["textures"] = [{"source": 0}]
        document["images"] = [image]
    return document, data


def text(document: dict) -> bytes:
    return json.dumps(document, indent=1).encode("utf-8")


def data_uri(data: bytes) -> str:
    return "data:application/octet-stream;base64," + base64.b64encode(data).decode("ascii")


def glb(document: dict, data: bytes) -> bytes:
    """A binary glTF: a header, a JSON chunk padded with spaces and a BIN chunk padded with 0."""
    body = text(document)
    body += b" " * (-len(body) % 4)
    data += b"\0" * (-len(data) % 4)
    chunks = struct.pack("<II", len(body), 0x4E4F534A) + body
    chunks += struct.pack("<II", len(data), 0x004E4942) + data
    return struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks


def models() -> dict:
    files = {}

    def inline(name: str, **options) -> None:
        document, data = gltf_json(**options)
        document["buffers"][0]["uri"] = data_uri(data)
        files["models/%s.gltf" % name] = text(document)

    inline("cube")
    inline("blue", color=BLUE)
    inline("rough", roughness=1.0)
    inline("metal", metallic=1.0, roughness=0.2)
    inline("moved", node={"mesh": 0, "translation": [10, 0, 0], "scale": [2, 2, 2]})

    document, data = gltf_json()
    files["models/cube.glb"] = glb(document, data)

    png = rf.png_rgba(TEXTURE)
    document, data = gltf_json(color=WHITE, textured=True, buffer_uri="tex.bin", image={"uri": "tex.png"})
    files["models/tex.gltf"] = text(document)
    files["models/tex.bin"] = data
    files["models/tex.png"] = png
    # The picture follows the geometry in the binary chunk, as a fifth buffer view.
    document, data = gltf_json(color=WHITE, textured=True,
                                image={"bufferView": 4, "mimeType": "image/png"})
    data += b"\0" * (-len(data) % 4)
    document["bufferViews"].append({"buffer": 0, "byteOffset": len(data), "byteLength": len(png)})
    document["buffers"][0]["byteLength"] = len(data) + len(png)
    files["models/tex.glb"] = glb(document, data + png)

    # Files that must fail to load.
    document, data = gltf_json(buffer_uri="absent.bin")
    files["models/bad/missing_bin.gltf"] = text(document)
    document, data = gltf_json(textured=True, image={"uri": "absent.png"})
    document["buffers"][0]["uri"] = data_uri(data)
    files["models/bad/missing_texture.gltf"] = text(document)
    document, data = gltf_json(mode=1)
    document["buffers"][0]["uri"] = data_uri(data)
    files["models/bad/lines.gltf"] = text(document)
    document, data = gltf_json(index_override=24)
    document["buffers"][0]["uri"] = data_uri(data)
    files["models/bad/index_range.gltf"] = text(document)
    files["models/bad/truncated.glb"] = files["models/cube.glb"][:40]
    files["models/bad/not_json.gltf"] = b"{ this is not json"
    return files


# --- The game ----------------------------------------------------------------------------------

WHITEN = """\
#include "mesh.hlsli"

cbuffer Uniforms : register(b0, space3) {
  MeshGlobals globals;
  float amount;
  float3 unused_padding;
};

float4 main(MeshFragment input) : SV_Target0 {
  const float4 color = base_color_at(globals, input.uv);
  return premultiply(lerp(color.rgb, float3(1.0, 1.0, 1.0), amount), color.a);
}
"""

CAMERA = 'camera("stage", { position = { 0, 0, 5 }, look_at = { 0, 0, 0 }, fov = 90 })'
AMBIENT = 'light("stage", "fill", { kind = "ambient", intensity = 0.1 })'


def label(name: str, *lines: str) -> str:
    body = "\n".join("  " + line for line in ("scene3d(\"stage\")", CAMERA) + lines)
    return 'label("%s", function()\n%s\n  narrate("framed")\nend)\n' % (name, body)


def sun(direction: str, intensity: float) -> str:
    return ('light("stage", "sun", { kind = "directional", direction = %s, intensity = %s })'
            % (direction, intensity))


def cube(path: str, shader: str, extra: str = "") -> str:
    return 'model("cube", "models/%s", { scene = "stage", shader = "%s"%s })' % (path, shader, extra)


def script() -> str:
    labels = [
        label("unlit",
              'model("near", "models/cube.gltf", { scene = "stage" })',
              'model("far", "models/blue.gltf", '
              '{ scene = "stage", position = { 0, 0, -4 }, scale = 3 })'),
        label("toon_bright", sun("{ 0, 0, -1 }", 0.8), AMBIENT, cube("cube.gltf", "toon")),
        label("toon_middle", sun("{ 0, -0.866, -0.5 }", 0.8), AMBIENT, cube("cube.gltf", "toon")),
        label("toon_dark", sun("{ 0, -0.995, -0.1 }", 0.8), AMBIENT, cube("cube.gltf", "toon")),
        label("toon_point",
              'light("stage", "bulb", { kind = "point", position = { 0, 0, 5 }, intensity = 8.5 })',
              AMBIENT, cube("cube.gltf", "toon")),
        label("pbr_rough", sun("{ 0, 0, -1 }", 0.5), AMBIENT, cube("rough.gltf", "pbr")),
        label("pbr_metal", sun("{ 0, 0, -1 }", 0.5), AMBIENT, cube("metal.gltf", "pbr")),
        label("textured", cube("tex.glb", "unlit")),
        label("custom", 'shader("whiten", { "amount" })',
              cube("cube.gltf", "whiten", ", uniforms = { amount = 0.5 }")),
    ]
    return "\n".join(labels)


def game() -> dict:
    files = {"scripts/start.lua": script().encode("utf-8"),
             "shaders/whiten.frag.hlsl": WHITEN.encode("utf-8")}
    files.update(models())
    return files


def golden_unlit() -> bytes:
    canvas = rf.Canvas(SIZE, SIZE, (0, 0, 0, 255))
    near = tuple(round(c * 255) for c in ORANGE) + (255,)
    far = tuple(round(c * 255) for c in BLUE) + (255,)
    canvas.draw(rf.WHITE, (16, 16, 32, 32), tint=far)  # drawn first: the near cube covers it
    canvas.draw(rf.WHITE, (24, 24, 16, 16), tint=near)
    return canvas.png()


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: model_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)
    for directory, files in (("game", game()), ("golden", {"unlit.png": golden_unlit()})):
        for name, content in files.items():
            path = out / directory / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
