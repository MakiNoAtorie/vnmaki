#!/usr/bin/env python3
"""Generate the loose-file trees the VFS unit tests mount.

Usage: vfs_fixtures.py OUT_DIR

OUT_DIR is deleted and recreated, so the output depends only on this script.
Layout:

  OUT_DIR/outside.txt   next to the mounts; no virtual path may reach it
  OUT_DIR/base/         the first mount (a game's loose files)
  OUT_DIR/patch/        a second mount stacked on top of base

The contents are spelled out here and repeated as literals in
tests/unit/vfs_test.cpp, which is the specification the tests check.
"""

import os
import shutil
import sys
from pathlib import Path

FILES = {
    "outside.txt": b"outside the mount root\n",
    "base/hello.txt": b"hello from base\n",
    "base/override.txt": b"base version\n",
    "base/empty.bin": b"",
    "base/bytes.bin": bytes(range(256)),
    "base/Case.txt": b"exact case\n",
    "base/dir/a.txt": b"a from base\n",
    "base/dir/shared.txt": b"shared from base\n",
    "base/dir/sub/b.txt": b"b from base\n",
    "patch/override.txt": b"patch version\n",
    "patch/dir/shared.txt": b"shared from patch\n",
    "patch/dir/c.txt": b"c from patch\n",
}

# A name that is not a valid virtual path segment. Windows cannot create it,
# so it only exists on other systems; listings must never show it.
POSIX_ONLY_FILES = {
    "base/dir/invalid?.txt": b"not reachable through the VFS\n",
}


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: vfs_fixtures.py OUT_DIR", file=sys.stderr)
        return 2
    out = Path(sys.argv[1])
    shutil.rmtree(out, ignore_errors=True)

    files = dict(FILES)
    if os.name != "nt":
        files.update(POSIX_ONLY_FILES)
    for relative, content in files.items():
        path = out / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
    return 0


if __name__ == "__main__":
    sys.exit(main())
