#!/usr/bin/env python3
"""Pack shadercross output into a `.vnshader` file (engine/render/precompiled_shader.hpp).

Usage: vnshader_pack.py OUT.vnshader REFLECTION.json SPIRV MSL DXIL

REFLECTION.json is what `shadercross -d JSON` wrote. Give an empty string for a format that was not
built (its block is then empty). The build uses this (cmake/VnmakiShaders.cmake,
vnmaki_add_precompiled_shader) to make a precompiled game shader in a build that has no run-time
shader compiler. It is a second writer of the format, so `tests/unit/import_test.cpp` checks that
the engine reads the result.
"""

import json
import struct
import sys
from pathlib import Path

MAGIC = b"VNSH"
VERSION = 1


def block(path: str) -> bytes:
    data = Path(path).read_bytes() if path else b""
    return struct.pack("<I", len(data)) + data


def main(out: str, reflection: str, spirv: str, msl: str, dxil: str) -> None:
    info = json.loads(Path(reflection).read_text())
    data = MAGIC + struct.pack("<III", VERSION, info["samplers"], info["uniform_buffers"])
    data += block(spirv) + block(msl) + block(dxil)
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    Path(out).write_bytes(data)


if __name__ == "__main__":
    if len(sys.argv) != 6:
        sys.exit(__doc__)
    main(*sys.argv[1:])
