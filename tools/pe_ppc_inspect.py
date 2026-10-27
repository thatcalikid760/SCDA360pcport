"""Inspect Xbox 360 binaries: XDVDFS ISO, XEX2 container, or unpacked PE (PowerPC BE). Stdlib only.

  python tools/pe_ppc_inspect.py <file.iso>                       list ISO tree
  python tools/pe_ppc_inspect.py <file.iso> --extract default.xex [--out assets]
  python tools/pe_ppc_inspect.py <file.xex|file.exe>              header/import/section report
"""
import os
import struct
import sys

SECTOR = 2048
XGD_BASES = (0, 0x18300000, 0xFD90000, 0x2080000)  # xiso, XGD3, XGD2, XGD1
XDVD_MAGIC = b"MICROSOFT*XBOX*MEDIA"

XEX_KEYS = {
    0x000002FF: "resource_info", 0x000003FF: "base_file_format", 0x00010001: "base_reference",
    0x00010100: "entry_point", 0x00010201: "image_base", 0x000103FF: "import_libraries",
    0x00018002: "checksum_timestamp", 0x000183FF: "original_pe_name", 0x000200FF: "static_libraries",
    0x00020104: "tls_info", 0x00020200: "default_stack_size", 0x00020301: "fs_cache_size",
    0x00020401: "default_heap_size", 0x00030000: "system_flags", 0x00040006: "execution_info",
    0x00040201: "title_workspace_size", 0x00040310: "game_ratings", 0x00040404: "lan_key",
    0x000405FF: "xbox360_logo", 0x00E10402: "alt_title_ids",
}
ENC = {0: "none", 1: "normal(AES)"}
COMP = {0: "none", 1: "basic", 2: "normal(LZX)", 3: "delta"}
PAGE_INFO = {1: "code", 2: "data", 3: "rodata"}


def u16(b, o): return struct.unpack_from(">H", b, o)[0]
def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def ver(v): return "%d.%d.%d.%d" % (v >> 28, (v >> 24) & 0xF, (v >> 8) & 0xFFFF, v & 0xFF)


# ---------- XDVDFS ----------
def find_xdvd(f):
    for base in XGD_BASES:
        f.seek(base + 32 * SECTOR)
        vd = f.read(28)
        if vd[:20] == XDVD_MAGIC:
            root_sec, root_size = struct.unpack_from("<II", vd, 20)
            return base, root_sec, root_size
    return None


def walk_dir(f, base, sec, size, prefix=""):
    f.seek(base + sec * SECTOR)
    data = f.read(size)
    out, stack, seen = [], [0], set()
    while stack:
        off = stack.pop()
        if off in seen or off * 4 + 14 > len(data):
            continue
        seen.add(off)
        left, right, esec, esize, attr, nlen = struct.unpack_from("<HHIIBB", data, off * 4)
        if left == 0xFFFF and right == 0xFFFF and esec == 0xFFFFFFFF:
            continue  # padding
        name = data[off * 4 + 14: off * 4 + 14 + nlen].decode("ascii", "replace")
        path = prefix + name
        if attr & 0x10:
            out.append((path + "/", esec, esize, True))
            if esize:
                out += walk_dir(f, base, esec, esize, path + "/")
        else:
            out.append((path, esec, esize, False))
        for nxt in (left, right):
            if nxt not in (0, 0xFFFF):
                stack.append(nxt)
    return out


def do_iso(path, extract, out_dir):
    with open(path, "rb") as f:
        hit = find_xdvd(f)
        if not hit:
            sys.exit("No XDVDFS volume found at known XGD offsets.")
        base, rsec, rsize = hit
        print("XDVDFS base=0x%X root_sector=%d root_size=%d" % (base, rsec, rsize))
        entries = sorted(walk_dir(f, base, rsec, rsize))
        if not extract:
            for p, s, n, d in entries:
                if p.count("/") <= (1 if d else 0) or p.lower().endswith((".xex", ".xexp")):
                    print("%s %12s  %s" % ("D" if d else "F", "" if d else n, p))
            print("(%d entries total; top level + all .xex shown)" % len(entries))
            return
        match = [e for e in entries if e[0].lower() == extract.lower() and not e[3]]
        if not match:
            sys.exit("Not found in ISO: %s" % extract)
        _, s, n, _ = match[0]
        os.makedirs(out_dir, exist_ok=True)
        dst = os.path.join(out_dir, os.path.basename(extract))
        f.seek(base + s * SECTOR)
        with open(dst, "wb") as o:
            left = n
            while left:
                chunk = f.read(min(left, 1 << 20))
                o.write(chunk)
                left -= len(chunk)
        print("Extracted %s (%d bytes) -> %s" % (extract, n, dst))


# ---------- XEX2 ----------
def do_xex(b):
    flags, pe_off, _, sec_off, nopt = struct.unpack_from(">IIIII", b, 4)
    print("XEX2 module_flags=0x%08X pe_data_offset=0x%X security_offset=0x%X opt_headers=%d"
          % (flags, pe_off, sec_off, nopt))
    opts = {}
    for i in range(nopt):
        k, v = struct.unpack_from(">II", b, 24 + i * 8)
        opts[k] = v
        print("  [0x%08X] %-22s 0x%08X" % (k, XEX_KEYS.get(k, "?"), v))

    if 0x000003FF in opts:
        o = opts[0x000003FF]
        e, c = u16(b, o + 4), u16(b, o + 6)
        print("base_file_format: encryption=%s compression=%s" % (ENC.get(e, e), COMP.get(c, c)))
    if 0x00040006 in opts:
        o = opts[0x00040006]
        media, v, bv, tid = struct.unpack_from(">IIII", b, o)
        plat, ext, disc, discs = b[o + 16:o + 20]
        print("execution_info: title_id=%08X media_id=%08X version=%s base=%s disc=%d/%d"
              % (tid, media, ver(v), ver(bv), disc, discs))
    if 0x000183FF in opts:
        o = opts[0x000183FF]
        print("original_pe_name:", b[o + 4:o + u32(b, o)].split(b"\0")[0].decode("ascii", "replace"))
    if 0x000002FF in opts:
        o = opts[0x000002FF]
        for r in range((u32(b, o) - 4) // 16):
            name, addr, size = struct.unpack_from(">8sII", b, o + 4 + r * 16)
            print("resource: %-8s addr=0x%08X size=0x%X" % (name.rstrip(b"\0").decode("ascii", "replace"), addr, size))
    if 0x000200FF in opts:
        o = opts[0x000200FF]
        print("static_libraries:")
        for r in range((u32(b, o) - 4) // 16):
            name, a, bb, c, d = struct.unpack_from(">8sHHHH", b, o + 4 + r * 16)
            print("  %-8s %d.%d.%d.%d" % (name.rstrip(b"\0").decode("ascii", "replace"), a, bb, c, d & 0xFF))
    if 0x000103FF in opts:
        o = opts[0x000103FF]
        total, strsz, nstr = struct.unpack_from(">III", b, o)
        names = [s.decode("ascii", "replace") for s in b[o + 12:o + 12 + strsz].split(b"\0") if s][:nstr]
        p, end = o + 12 + strsz, o + total
        print("import_libraries (%d):" % nstr)
        while p < end:
            lsize = u32(b, p)
            v, vmin = u32(b, p + 28), u32(b, p + 32)
            idx, cnt = u16(b, p + 36), u16(b, p + 38)
            print("  %-14s ver=%s min=%s records=%d" % (names[idx] if idx < len(names) else idx, ver(v), ver(vmin), cnt))
            p += lsize

    s = sec_off
    img_size, img_flags, load = u32(b, s + 4), u32(b, s + 0x10C), u32(b, s + 0x110)
    npages = u32(b, s + 0x180)
    page = 0x1000 if img_flags & 0x10000000 else 0x10000
    print("security: image_size=0x%X load_address=0x%08X image_flags=0x%08X page=0x%X descriptors=%d"
          % (img_size, load, img_flags, page, npages))
    addr, runs = load, []
    for i in range(npages):
        val = u32(b, s + 0x184 + i * 24)
        n, kind = val >> 4, PAGE_INFO.get(val & 0xF, "?%d" % (val & 0xF))
        if runs and runs[-1][0] == kind and runs[-1][2] == addr:
            runs[-1][2] += n * page
        else:
            runs.append([kind, addr, addr + n * page])
        addr += n * page
    for kind, a, e in runs:
        print("  %-6s 0x%08X-0x%08X (0x%X)" % (kind, a, e, e - a))


# ---------- PE (unpacked basefile) ----------
def do_pe(b):
    lfa = struct.unpack_from("<I", b, 0x3C)[0]
    if b[lfa:lfa + 4] != b"PE\0\0":
        sys.exit("MZ without PE signature.")
    mach, nsec, ts, _, _, optsz, ch = struct.unpack_from("<HHIIIHH", b, lfa + 4)
    opt = lfa + 24
    magic = struct.unpack_from("<H", b, opt)[0]
    entry = struct.unpack_from("<I", b, opt + 16)[0]
    base = struct.unpack_from("<I", b, opt + 28)[0] if magic == 0x10B else struct.unpack_from("<Q", b, opt + 24)[0]
    print("PE machine=0x%04X%s sections=%d timestamp=0x%08X image_base=0x%X entry_rva=0x%X"
          % (mach, " (PowerPC BE)" if mach == 0x1F2 else "", nsec, ts, base, entry))
    for i in range(nsec):
        o = opt + optsz + i * 40
        name, vsz, va, rsz, rptr = struct.unpack_from("<8sIIII", b, o)
        chars = struct.unpack_from("<I", b, o + 36)[0]
        print("  %-8s va=0x%08X vsize=0x%08X raw=0x%08X@0x%X flags=0x%08X%s"
              % (name.rstrip(b"\0").decode("ascii", "replace"), base + va, vsz, rsz, rptr, chars,
                 " CODE" if chars & 0x20 else ""))


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    path = sys.argv[1]
    if path.lower().endswith(".iso"):
        a = sys.argv[2:]
        ext = a[a.index("--extract") + 1] if "--extract" in a else None
        out = a[a.index("--out") + 1] if "--out" in a else "assets"
        return do_iso(path, ext, out)
    with open(path, "rb") as f:
        b = f.read()
    if b[:4] == b"XEX2":
        do_xex(b)
    elif b[:2] == b"MZ":
        do_pe(b)
    else:
        sys.exit("Unknown format (magic %r)." % b[:4])


if __name__ == "__main__":
    main()
