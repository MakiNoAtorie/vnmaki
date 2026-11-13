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
  skinned.gltf a bar of two segments with two joints ("root" at the origin, "tip" 1 above it) and
               the clips bend, flip, snap and slide (see skinned() below)
  morph.gltf   a quad with the morph targets mouth_open and smile, and the clip open
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
import math
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


# --- Skins, morph targets and clips (ticket 47) -----------------------------------------------

FLOAT_ACCESSOR = 5126
UNSIGNED_BYTE = 5121


class Gltf:
    """A glTF document with one buffer: add() appends data and returns the accessor's index."""

    def __init__(self) -> None:
        self.data = bytearray()
        self.views: list = []
        self.accessors: list = []

    def add(self, blob: bytes, component: int, count: int, kind: str, target=None, **extra) -> int:
        view = {"buffer": 0, "byteOffset": len(self.data), "byteLength": len(blob)}
        if target is not None:
            view["target"] = target
        self.data += blob + b"\0" * (-len(blob) % 4)
        self.views.append(view)
        self.accessors.append({"bufferView": len(self.views) - 1, "componentType": component,
                               "count": count, "type": kind, **extra})
        return len(self.accessors) - 1

    def floats(self, values, kind: str, width: int, target=None, bounds=False) -> int:
        flat = [x for value in values for x in (value if hasattr(value, "__len__") else (value,))]
        extra = {}
        if bounds:
            columns = list(zip(*values)) if width > 1 else [list(values)]
            extra = {"min": [min(c) for c in columns], "max": [max(c) for c in columns]}
        return self.add(struct.pack("<%df" % len(flat), *flat), FLOAT_ACCESSOR, len(values), kind,
                        target, **extra)

    def document(self, **parts) -> dict:
        return {"asset": {"version": "2.0", "generator": "model_fixtures.py"},
                "buffers": [{"byteLength": len(self.data), "uri": data_uri(bytes(self.data))}],
                "bufferViews": self.views, "accessors": self.accessors, **parts}


def quat_z(degrees: float) -> tuple:
    half = math.radians(degrees) / 2
    return (0.0, 0.0, math.sin(half), math.cos(half))


BAR_HALF = 0.25
BAR_RINGS = (0.0, 1.0, 2.0)  # heights of the three rings of four vertices; the tip joint is at 1
CORNERS = ((-BAR_HALF, -BAR_HALF), (BAR_HALF, -BAR_HALF), (BAR_HALF, BAR_HALF), (-BAR_HALF, BAR_HALF))
ORANGE_MATERIAL = [{"pbrMetallicRoughness": {"baseColorFactor": [*ORANGE, 1.0],
                                              "metallicFactor": 0.0, "roughnessFactor": 0.5}}]


def skinned(joint_override=None, times_override=None) -> dict:
    """A bar along y (0 to 2, half-width 0.25) with the joints root (node 0, at the origin) and
    tip (node 1, a child of root, 1 above it). The ring at y = 0 follows root, the ring at y = 2
    follows tip, and the ring at y = 1 follows both by half. The mesh is node 2, skin 0.

    Clips (each a time in seconds and a value, linear unless it says another):
      bend   tip turns about z from 0 to 90 degrees, times 0 to 1
      flip   tip is turned -90 degrees, times 0 and 1
      snap   root moves 0, 1, 2 up at times 0, 1, 2 (step)
      slide  root moves 0 to 1 along x in a second, cubic spline with an out-tangent of 4 at 0
    """
    g = Gltf()
    positions = [(x, y, z) for y in BAR_RINGS for x, z in CORNERS]
    k = 1 / math.sqrt(2)
    normals = [(math.copysign(k, x), 0.0, math.copysign(k, z)) for _ in BAR_RINGS for x, z in CORNERS]
    joints = [(0, 0, 0, 0)] * 4 + [(0, 1, 0, 0)] * 4 + [(1, 0, 0, 0)] * 4
    if joint_override is not None:
        joints[0] = (joint_override, 0, 0, 0)
    weights = [(1.0, 0.0, 0.0, 0.0)] * 4 + [(0.5, 0.5, 0.0, 0.0)] * 4 + [(1.0, 0.0, 0.0, 0.0)] * 4
    indices = []
    for segment in range(2):
        for c in range(4):
            a, b = segment * 4 + c, segment * 4 + (c + 1) % 4
            indices += [a, b, b + 4, a, b + 4, a + 4]
    attributes = {
        "POSITION": g.floats(positions, "VEC3", 3, ARRAY_BUFFER, bounds=True),
        "NORMAL": g.floats(normals, "VEC3", 3, ARRAY_BUFFER),
        "JOINTS_0": g.add(b"".join(struct.pack("<4B", *j) for j in joints), UNSIGNED_BYTE,
                           len(joints), "VEC4", ARRAY_BUFFER),
        "WEIGHTS_0": g.floats(weights, "VEC4", 4, ARRAY_BUFFER),
    }
    index_accessor = g.add(b"".join(struct.pack("<H", i) for i in indices), UNSIGNED_SHORT,
                           len(indices), "SCALAR", ELEMENT_ARRAY_BUFFER)
    identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
    below = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]  # column-major: moves down by 1
    inverse_bind = g.floats([identity, below], "MAT4", 16)

    def clip(name, path, interpolation, times, values, kind, node):
        # Only the first clip takes the damaged times: the others keep their key counts.
        damaged = times_override is not None and name == "bend"
        time_accessor = g.floats(times_override if damaged else times, "SCALAR", 1, bounds=True)
        return {"name": name,
                "samplers": [{"input": time_accessor, "output": g.floats(values, kind, 0),
                              "interpolation": interpolation}],
                "channels": [{"sampler": 0, "target": {"node": node, "path": path}}]}

    zero = (0.0, 0.0, 0.0)
    animations = [
        clip("bend", "rotation", "LINEAR", [0.0, 1.0], [(0.0, 0.0, 0.0, 1.0), quat_z(90)], "VEC4", 1),
        clip("flip", "rotation", "LINEAR", [0.0, 1.0], [quat_z(-90), quat_z(-90)], "VEC4", 1),
        clip("snap", "translation", "STEP", [0.0, 1.0, 2.0],
             [zero, (0.0, 1.0, 0.0), (0.0, 2.0, 0.0)], "VEC3", 0),
        clip("slide", "translation", "CUBICSPLINE", [0.0, 1.0],
             [zero, zero, (4.0, 0.0, 0.0), zero, (1.0, 0.0, 0.0), zero], "VEC3", 0),
    ]
    return g.document(
        scene=0, scenes=[{"nodes": [0, 2]}],
        nodes=[{"name": "root", "children": [1]}, {"name": "tip", "translation": [0, 1, 0]},
               {"name": "bar", "mesh": 0, "skin": 0}],
        skins=[{"joints": [0, 1], "inverseBindMatrices": inverse_bind, "skeleton": 0}],
        meshes=[{"primitives": [{"attributes": attributes, "indices": index_accessor,
                                 "material": 0}]}],
        materials=ORANGE_MATERIAL, animations=animations)


def morph() -> dict:
    """A quad in the xy plane (-1 to 1, 0 to 1) with two morph targets that move its vertices:
    mouth_open raises the top edge by 1, smile spreads the bottom edge by 0.5 each way. The weights
    start at 0. The clip open takes both weights from 0 to (1, 0.5) in a second."""
    g = Gltf()
    positions = [(-1.0, 0.0, 0.0), (1.0, 0.0, 0.0), (1.0, 1.0, 0.0), (-1.0, 1.0, 0.0)]
    zero = (0.0, 0.0, 0.0)
    open_deltas = [zero, zero, (0.0, 1.0, 0.0), (0.0, 1.0, 0.0)]
    smile_deltas = [(-0.5, 0.0, 0.0), (0.5, 0.0, 0.0), zero, zero]
    attributes = {"POSITION": g.floats(positions, "VEC3", 3, ARRAY_BUFFER, bounds=True),
                  "NORMAL": g.floats([(0.0, 0.0, 1.0)] * 4, "VEC3", 3, ARRAY_BUFFER)}
    index_accessor = g.add(b"".join(struct.pack("<H", i) for i in (0, 1, 2, 0, 2, 3)),
                           UNSIGNED_SHORT, 6, "SCALAR", ELEMENT_ARRAY_BUFFER)
    targets = [{"POSITION": g.floats(open_deltas, "VEC3", 3, bounds=True)},
               {"POSITION": g.floats(smile_deltas, "VEC3", 3, bounds=True)}]
    times = g.floats([0.0, 1.0], "SCALAR", 1, bounds=True)
    values = g.floats([0.0, 0.0, 1.0, 0.5], "SCALAR", 1)
    return g.document(
        scene=0, scenes=[{"nodes": [0]}], nodes=[{"name": "mouth", "mesh": 0}],
        meshes=[{"primitives": [{"attributes": attributes, "indices": index_accessor,
                                 "material": 0, "targets": targets}],
                 "weights": [0.0, 0.0], "extras": {"targetNames": ["mouth_open", "smile"]}}],
        materials=ORANGE_MATERIAL,
        animations=[{"name": "open", "samplers": [{"input": times, "output": values,
                                                    "interpolation": "LINEAR"}],
                     "channels": [{"sampler": 0, "target": {"node": 0, "path": "weights"}}]}])


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
    files["models/skinned.gltf"] = text(skinned())
    files["models/morph.gltf"] = text(morph())

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
    files["models/bad/joint_range.gltf"] = text(skinned(joint_override=9))
    files["models/bad/time_back.gltf"] = text(skinned(times_override=[1.0, 0.0]))
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


def bent_arm() -> str:
    """The bar bent 90 degrees at the tip joint, seen from the front by a camera that looks at the
    joint. The bar goes up from y = 0 to the joint, and the arm goes from there to the left."""
    return ('label("skinned_bent", function()\n  scene3d("stage")\n'
            '  camera("stage", { position = { 0, 1, 5 }, look_at = { 0, 1, 0 }, fov = 90 })\n'
            '  model("arm", "models/skinned.gltf", { scene = "stage" })\n'
            '  play("@3d.stage.arm", "bend", { seek = 1 })\n  narrate("framed")\nend)\n')


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
        bent_arm(),
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
