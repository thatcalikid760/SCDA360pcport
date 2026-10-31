"""Inflate an SC:DA .lin linker file (a sequence of zlib streams) into one flat file.

usage: python -I tools/lin_inflate.py <file.lin> <out.bin>

Reverse-engineering aid for the game's own packages (menus, UnrealScript). The output is game data:
keep it under _run/ (git-ignored), never commit it.
"""
import sys
import zlib


def inflate(data):
    out = bytearray()
    pos = 0
    while True:
        # zlib header: 0x78 then any of the four compression levels' FLG bytes
        starts = [s for s in (data.find(h, pos) for h in (b"\x78\x01", b"\x78\x5e", b"\x78\x9c", b"\x78\xda")) if s >= 0]
        if not starts:
            break
        start = min(starts)
        d = zlib.decompressobj()
        try:
            out += d.decompress(data[start:])
        except zlib.error:
            pos = start + 2
            continue
        pos = len(data) - len(d.unused_data)
    return bytes(out)


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    flat = inflate(open(src, "rb").read())
    open(dst, "wb").write(flat)
    print(f"{src}: {len(flat)} bytes inflated -> {dst}")
