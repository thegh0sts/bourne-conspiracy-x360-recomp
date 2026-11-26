"""Reticle size: the aiming reticle's size in the game's HUD.

The reticle is a UIImage, BourneHUD.HudUI.HUDContainer.Reticule, cooked into
BourneGame\\CookedXenon\\BourneNative.xxx: 40 x 40 (Position Value[2] / Value[3], viewport pixels
of the game's 1280 x 720 HUD), docked by its left and top faces to the HUD's CenterAlign widget
with a padding of -20 each, so it sits centred. Its size comes from nowhere else (no config key),
so the launcher writes the size into a copy of the package: 40 x k and -20 x k.

The package is LZO-compressed in chunks of 1 MB, each in 128 KB blocks. The four floats sit in one
block; that block is decompressed (a small LZO1X decoder below), patched, and stored back as an
uncompressed LZO1X stream (one literal run, which any LZO1X decoder reads), and the chunk table
and following chunks' offsets are moved by the size difference. Xbox360TOC.txt gets the new size
and md5. Everything else in the file is copied unchanged. The original stays as
BourneNative.xxx.bak; at 100% it is put back.
"""

import hashlib
import shutil
import struct
from pathlib import Path

PACKAGE_REL = Path("BourneGame") / "CookedXenon" / "BourneNative.xxx"
TOC_NAME = r"..\BourneGame\CookedXenon\BourneNative.xxx"
PACKAGE_TAG = 0x9E2A83C1
COMPRESS_LZO = 2

# Offsets in the uncompressed package (retail BourneNative.xxx, 14712832 bytes), and the
# original value at each: Position Value[2], Value[3] (width, height) and DockPadding
# PaddingValue[0], [1] (left, top).
RETICLE_FLOATS = (
    (0x2823E66, 40.0),
    (0x2823E82, 40.0),
    (0x2823D86, -20.0),
    (0x2823DA2, -20.0),
)


# --------------------------------------------------------------------------
# LZO1X
# --------------------------------------------------------------------------

def lzo1x_decompress(src, out_len):
    out = bytearray()
    ip = 0
    state = 0

    def ext(base):
        nonlocal ip
        n = 0
        while src[ip] == 0:
            n += 255
            ip += 1
        n += base + src[ip]
        ip += 1
        return n

    def copy(dist, length):
        s = len(out) - dist
        if s < 0:
            raise ValueError("lzo: bad distance")
        if dist >= length:
            out.extend(out[s:s + length])
        else:
            for k in range(length):
                out.append(out[s + k])

    t = src[0]
    if t > 17:
        ip = 1
        t -= 17
        out += src[ip:ip + t]
        ip += t
        state = 4 if t >= 4 else t
    while True:
        t = src[ip]
        ip += 1
        if t >= 64:
            dist = 1 + ((t >> 2) & 7) + (src[ip] << 3)
            ip += 1
            copy(dist, (t >> 5) + 1)
        elif t >= 32:
            length = t & 31
            if length == 0:
                length = ext(31)
            v = src[ip] | (src[ip + 1] << 8)
            ip += 2
            copy(1 + (v >> 2), length + 2)
        elif t >= 16:
            length = t & 7
            if length == 0:
                length = ext(7)
            v = src[ip] | (src[ip + 1] << 8)
            ip += 2
            dist = ((t & 8) << 11) + (v >> 2)
            if dist == 0:
                break  # end of stream
            copy(dist + 0x4000, length + 2)
        elif state == 0:
            length = (ext(15) if t == 0 else t) + 3
            out += src[ip:ip + length]
            ip += length
            state = 4
            continue
        elif state < 4:
            dist = 1 + (t >> 2) + (src[ip] << 2)
            ip += 1
            copy(dist, 2)
        else:
            dist = 1 + 0x800 + (t >> 2) + (src[ip] << 2)
            ip += 1
            copy(dist, 3)
        n = src[ip - 2] & 3
        out += src[ip:ip + n]
        ip += n
        state = n
    if len(out) != out_len:
        raise ValueError(f"lzo: {len(out)} bytes, expected {out_len}")
    return bytes(out)


def lzo1x_store(data):
    """An LZO1X stream holding data as one literal run (no compression)."""
    n = len(data)
    if n == 0:
        return b"\x11\x00\x00"
    if n <= 238:
        head = bytes([17 + n])
    else:
        m = n - 18
        z = (m - 1) // 255
        head = b"\x00" * (1 + z) + bytes([m - 255 * z])
    return head + bytes(data) + b"\x11\x00\x00"


# --------------------------------------------------------------------------
# Package
# --------------------------------------------------------------------------

def _chunk_table(d):
    """(offset of the chunk table, [(uncomp offset, uncomp size, comp offset, comp size)])."""
    if struct.unpack_from(">I", d, 0)[0] != PACKAGE_TAG:
        raise ValueError("not an Unreal package")
    o = 12
    n = struct.unpack_from(">i", d, o)[0]  # folder name
    o += 4 + (n if n >= 0 else -2 * n)
    o += 32 + 16  # flags, name / export / import tables, depends; GUID
    gens = struct.unpack_from(">i", d, o)[0]
    o += 4 + 12 * gens
    _, _, cflags, count = struct.unpack_from(">4I", d, o)
    o += 16
    if cflags != COMPRESS_LZO:
        raise ValueError(f"unexpected compression {cflags}")
    return o, [struct.unpack_from(">4I", d, o + 16 * i) for i in range(count)]


def patch_package(d: bytes, edits):
    """The package d with edits [(uncompressed offset, bytes)] applied; all in one block."""
    table_at, chunks = _chunk_table(d)
    first = min(off for off, _ in edits)
    last = max(off + len(b) for off, b in edits)
    for ci, (uo, us, co, cs) in enumerate(chunks):
        if uo <= first < uo + us:
            break
    else:
        raise ValueError("offset outside the package's chunks")
    tag, bsize, csz, usz = struct.unpack_from(">4I", d, co)
    o = co + 16
    blocks, t = [], 0
    while t < usz:
        c, u = struct.unpack_from(">2I", d, o)
        o += 8
        blocks.append([c, u])
        t += u
    data_at = o
    pos, start = uo, data_at
    for bi, (c, u) in enumerate(blocks):
        if pos <= first < pos + u:
            break
        pos += u
        start += c
    else:
        raise ValueError("offset outside the chunk's blocks")
    if last > pos + u:
        raise ValueError("edits span two blocks")
    block = bytearray(lzo1x_decompress(d[start:start + c], u))
    for off, b in edits:
        block[off - pos:off - pos + len(b)] = b
    new = lzo1x_store(block)
    delta = len(new) - c
    blocks[bi][0] = len(new)

    head = bytearray(struct.pack(">4I", tag, bsize, csz + delta, usz))
    for bc, bu in blocks:
        head += struct.pack(">2I", bc, bu)
    chunk = bytes(head) + d[data_at:start] + new + d[start + c:co + cs]

    out = bytearray(d[:co]) + chunk + d[co + cs:]
    for cj, (ju, jus, jo, js) in enumerate(chunks):
        if cj == ci:
            js += delta
        elif jo > co:
            jo += delta
        struct.pack_into(">4I", out, table_at + 16 * cj, ju, jus, jo, js)
    return bytes(out)


def read_floats(d: bytes):
    """The reticle's four floats as stored in package d (uncompressed offsets)."""
    _, chunks = _chunk_table(d)
    out = []
    for off, _ in RETICLE_FLOATS:
        for uo, us, co, cs in chunks:
            if uo <= off < uo + us:
                break
        tag, bsize, csz, usz = struct.unpack_from(">4I", d, co)
        o = co + 16
        blocks, t = [], 0
        while t < usz:
            c, u = struct.unpack_from(">2I", d, o)
            o += 8
            blocks.append((c, u))
            t += u
        pos, start = uo, o
        for c, u in blocks:
            if pos <= off < pos + u:
                block = lzo1x_decompress(d[start:start + c], u)
                out.append(struct.unpack_from(">f", block, off - pos)[0])
                break
            pos += u
            start += c
    return out


def _update_toc(toc: Path, size: int, md5: str):
    lines = toc.read_bytes().decode("latin-1").split("\n")
    for i, line in enumerate(lines):
        parts = line.rstrip("\r").split(" ")
        if len(parts) >= 5 and parts[3].lower() == TOC_NAME.lower():
            parts[0], parts[4] = str(size), md5
            lines[i] = " ".join(parts) + ("\r" if line.endswith("\r") else "")
            break
    else:
        raise ValueError("BourneNative.xxx not listed in Xbox360TOC.txt")
    toc.write_bytes("\n".join(lines).encode("latin-1"))


def apply_reticle_size(game: Path, toc: Path, percent: int) -> str:
    """Write the reticle at percent of its size into game's BourneNative.xxx (from its .bak) and
    its toc entry; at 100 put the original back. toc is updated in place (call after the config's
    own TOC update). Returns a short note for the log, or raises ValueError."""
    pkg = game / PACKAGE_REL
    bak = pkg.with_name(pkg.name + ".bak")
    if percent == 100:
        if bak.exists():
            shutil.copy2(bak, pkg)
            data = bak.read_bytes()
            _update_toc(toc, len(data), hashlib.md5(data).hexdigest())
        return "reticle: original size"
    if not bak.exists():
        shutil.copy2(pkg, bak)
    d = bak.read_bytes()
    current = read_floats(d)
    if [round(v, 3) for v in current] != [v for _, v in RETICLE_FLOATS]:
        raise ValueError(f"reticle: unexpected values {current} (different game version?)")
    k = percent / 100
    out = patch_package(d, [(off, struct.pack(">f", v * k)) for off, v in RETICLE_FLOATS])
    pkg.write_bytes(out)
    _update_toc(toc, len(out), hashlib.md5(out).hexdigest())
    return f"reticle: {percent}% ({40 * k:g} px)"
