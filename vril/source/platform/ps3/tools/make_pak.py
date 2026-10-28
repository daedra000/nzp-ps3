#!/usr/bin/env python3
"""Moves NZ:P game data folders into a Quake pak0.pak, for the PS3 PKG.

    make_pak.py GAMEDIR FOLDER [FOLDER ...]

Packs every file under GAMEDIR/FOLDER... into GAMEDIR/pak0.pak (names relative to
GAMEDIR, forward slashes) and removes the loose copies. A PS3 PKG install drops
entries once a package holds too many of them, and NZ:P's data is over 1100 files:
in one pak the PKG holds a few dozen. Quake PAK format: "PACK", directory offset and
length (little-endian), the file data, then 64-byte entries (56-byte name, offset,
length).
"""
import os
import shutil
import struct
import sys


def main():
    gamedir = sys.argv[1]
    folders = sys.argv[2:]
    entries = []
    for folder in folders:
        root = os.path.join(gamedir, folder)
        if not os.path.isdir(root):
            continue
        for dirpath, _, files in os.walk(root):
            for name in sorted(files):
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, gamedir).replace(os.sep, "/")
                if len(rel.encode()) >= 56:
                    sys.exit("make_pak: name too long for a pak entry: " + rel)
                entries.append((rel, path))
    if len(entries) > 2048:   # MAX_FILES_IN_PACK in platform/ps3/common.c
        sys.exit("make_pak: %d files, the engine reads at most 2048" % len(entries))

    out = os.path.join(gamedir, "pak0.pak")
    directory = b""
    with open(out, "wb") as f:
        f.write(b"\0" * 12)
        for rel, path in entries:
            data = open(path, "rb").read()
            pos = f.tell()
            f.write(data)
            directory += struct.pack("<56sii", rel.encode(), pos, len(data))
        dirofs = f.tell()
        f.write(directory)
        f.seek(0)
        f.write(struct.pack("<4sii", b"PACK", dirofs, len(directory)))

    for folder in folders:
        shutil.rmtree(os.path.join(gamedir, folder), ignore_errors=True)
    print("pak0.pak: %d files, %d bytes" % (len(entries), os.path.getsize(out)))


if __name__ == "__main__":
    main()
