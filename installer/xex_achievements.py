"""The game's achievements straight from default.xex, for the launcher's Achievements tab before the
game has ever run.

The achievements, their text in every language and their icons live in the title's XDBF, a resource
inside the XEX named after the title ID. Retail XEXs are encrypted (AES-128-CBC with a per-title
key, itself encrypted with the retail key) and "basic" compressed (runs of data and zero fill), so
only the bytes holding that resource are decrypted, by a small pure-Python AES (no extra packages
in the launcher). Writes the same <title id>-list.json and icons/<image id>.png the game writes
(src/achievement_list.cpp), so whichever ran last wins and both read the same.
"""

import json
import struct
from pathlib import Path

LANGUAGE_IDS = (1, 3, 4, 5, 6)  # English, German, French, Spanish, Italian (XLanguage)

# ---------------------------------------------------------------------------------------------
# AES-128 decryption (FIPS-197), table-driven.

_SBOX = [0] * 256
_INV = [0] * 256


def _build_tables():
    # Multiplicative inverses in GF(2^8) through exp/log tables (generator 3), then the affine map.
    exp, log = [0] * 255, [0] * 256
    x = 1
    for i in range(255):
        exp[i] = x
        log[x] = i
        x ^= ((x << 1) ^ (0x1B if x & 0x80 else 0)) & 0xFF
    for v in range(256):
        inv = exp[(255 - log[v]) % 255] if v else 0
        y = inv
        for shift in (1, 2, 3, 4):
            y ^= ((inv << shift) | (inv >> (8 - shift))) & 0xFF
        y ^= 0x63
        _SBOX[v] = y
        _INV[y] = v


_build_tables()


def _mul(a, b):
    r = 0
    while b:
        if b & 1:
            r ^= a
        a = ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1
        b >>= 1
    return r


# Inverse round tables: InvSubBytes then InvMixColumns for one byte in each row.
_TD = [[0] * 256 for _ in range(4)]
for _x in range(256):
    _s = _INV[_x]
    _w = (_mul(_s, 14) << 24) | (_mul(_s, 9) << 16) | (_mul(_s, 13) << 8) | _mul(_s, 11)
    for _r in range(4):
        _TD[_r][_x] = ((_w >> (8 * _r)) | (_w << (32 - 8 * _r))) & 0xFFFFFFFF


def _expand_key(key):
    w = list(struct.unpack(">4I", key))
    rcon = 1
    for i in range(4, 44):
        t = w[i - 1]
        if i % 4 == 0:
            t = ((t << 8) | (t >> 24)) & 0xFFFFFFFF
            t = (_SBOX[t >> 24] << 24) | (_SBOX[(t >> 16) & 0xFF] << 16) | (_SBOX[(t >> 8) & 0xFF] << 8) | _SBOX[t & 0xFF]
            t ^= rcon << 24
            rcon = _mul(rcon, 2)
        w.append(w[i - 4] ^ t)
    # Decryption key schedule: rounds reversed, InvMixColumns on the middle ones.
    rounds = [w[4 * r:4 * r + 4] for r in range(11)][::-1]
    dk = list(rounds[0])
    for r in range(1, 10):
        for v in rounds[r]:
            dk.append(_TD[0][_SBOX[v >> 24]] ^ _TD[1][_SBOX[(v >> 16) & 0xFF]] ^ _TD[2][_SBOX[(v >> 8) & 0xFF]]
                      ^ _TD[3][_SBOX[v & 0xFF]])
    dk += rounds[10]
    return dk


def _decrypt_block(dk, block):
    t0, t1, t2, t3 = _TD
    s0, s1, s2, s3 = struct.unpack(">4I", block)
    s0 ^= dk[0]; s1 ^= dk[1]; s2 ^= dk[2]; s3 ^= dk[3]
    k = 4
    for _ in range(9):
        a0 = t0[s0 >> 24] ^ t1[(s3 >> 16) & 0xFF] ^ t2[(s2 >> 8) & 0xFF] ^ t3[s1 & 0xFF] ^ dk[k]
        a1 = t0[s1 >> 24] ^ t1[(s0 >> 16) & 0xFF] ^ t2[(s3 >> 8) & 0xFF] ^ t3[s2 & 0xFF] ^ dk[k + 1]
        a2 = t0[s2 >> 24] ^ t1[(s1 >> 16) & 0xFF] ^ t2[(s0 >> 8) & 0xFF] ^ t3[s3 & 0xFF] ^ dk[k + 2]
        a3 = t0[s3 >> 24] ^ t1[(s2 >> 16) & 0xFF] ^ t2[(s1 >> 8) & 0xFF] ^ t3[s0 & 0xFF] ^ dk[k + 3]
        s0, s1, s2, s3 = a0, a1, a2, a3
        k += 4
    inv = _INV
    out = []
    for a, b, c, d, kw in ((s0, s3, s2, s1, dk[40]), (s1, s0, s3, s2, dk[41]),
                           (s2, s1, s0, s3, dk[42]), (s3, s2, s1, s0, dk[43])):
        out.append(((inv[a >> 24] << 24) | (inv[(b >> 16) & 0xFF] << 16) | (inv[(c >> 8) & 0xFF] << 8)
                    | inv[d & 0xFF]) ^ kw)
    return struct.pack(">4I", *out)


def aes128_ecb_decrypt(key, data):
    dk = _expand_key(key)
    return b"".join(_decrypt_block(dk, data[i:i + 16]) for i in range(0, len(data), 16))


def aes128_cbc_decrypt(key, iv, data):
    dk = _expand_key(key)
    out = bytearray()
    prev = iv
    for i in range(0, len(data), 16):
        c = data[i:i + 16]
        p = _decrypt_block(dk, c)
        out += bytes(x ^ y for x, y in zip(p, prev))
        prev = c
    return bytes(out)


# ---------------------------------------------------------------------------------------------
# XEX

RETAIL_KEY = bytes.fromhex("20B185A59D28FDC340583FBB0896BF91")
DEVKIT_KEY = bytes(16)


def _headers(f):
    head = f.read(0x18)
    if head[:4] != b"XEX2":
        raise ValueError("not an XEX2 file")
    data_offset, _, security_offset, count = struct.unpack(">IIII", head[8:0x18])
    f.seek(0)
    head = f.read(data_offset)
    fields = {}
    for i in range(count):
        key, value = struct.unpack(">II", head[0x18 + i * 8:0x20 + i * 8])
        fields[key] = value
    return head, data_offset, security_offset, fields


def read_xdbf(xex_path):
    """The title's XDBF bytes and its title ID, or (None, title_id) when there is none."""
    with open(xex_path, "rb") as f:
        head, data_offset, security_offset, fields = _headers(f)
        title_id = struct.unpack(">I", head[fields[0x40006] + 12:fields[0x40006] + 16])[0] if 0x40006 in fields else 0
        image_base = fields.get(0x10201, 0x82000000)
        # Resources (0x2FF): a size then entries of name[8], address, size.
        if 0x2FF not in fields:
            return None, title_id
        at = fields[0x2FF]
        size = struct.unpack(">I", head[at:at + 4])[0]
        resource = None
        for i in range((size - 4) // 16):
            e = head[at + 4 + i * 16:at + 20 + i * 16]
            name = e[:8].rstrip(b"\0").decode("ascii", "replace")
            address, length = struct.unpack(">II", e[8:16])
            if name.upper() == f"{title_id:08X}":
                resource = (address - image_base, length)
        if resource is None:
            return None, title_id
        # File format (0x3FF): encryption and compression.
        at = fields[0x3FF]
        info_size, encryption, compression = struct.unpack(">IHH", head[at:at + 8])
        if compression == 1:  # basic: (data size, zero size) runs
            runs = [struct.unpack(">II", head[at + 8 + i * 8:at + 16 + i * 8]) for i in range((info_size - 8) // 8)]
        elif compression == 0:
            runs = [(1 << 31, 0)]
        else:
            raise ValueError("this XEX is compressed in a way the launcher can't read")
        start, length = resource
        # Where the resource's bytes are in the (encrypted) payload: the image is the runs' data
        # with their zero fill in between.
        pieces = []  # (payload offset, image offset, size)
        image = payload = 0
        for data_size, zero_size in runs:
            lo, hi = max(start, image), min(start + length, image + data_size)
            if lo < hi:
                pieces.append((payload + lo - image, lo, hi - lo))
            image += data_size + zero_size
            payload += data_size
        if not pieces:
            return None, title_id
        first = pieces[0][0] // 16 * 16
        last = -(-(pieces[-1][0] + pieces[-1][2]) // 16) * 16
        f.seek(data_offset + max(0, first - 16))
        raw = f.read(last - first + (16 if first else 0))
        if encryption == 1:
            iv, raw = (raw[:16], raw[16:]) if first else (bytes(16), raw)
            f.seek(security_offset + 0x150)
            file_key = f.read(16)
            plain = None
            for master in (RETAIL_KEY, DEVKIT_KEY):
                session = aes128_ecb_decrypt(master, file_key)
                p0 = aes128_cbc_decrypt(session, iv, raw[:16])
                if pieces[0][1] == start and p0[pieces[0][0] - first:pieces[0][0] - first + 4] == b"XDBF":
                    plain = p0 + aes128_cbc_decrypt(session, raw[:16], raw[16:])
                    break
            if plain is None:
                return None, title_id
        else:
            plain = raw[16:] if first else raw
        out = bytearray(length)
        for p, i, n in pieces:
            out[i - start:i - start + n] = plain[p - first:p - first + n]
        return bytes(out), title_id


# ---------------------------------------------------------------------------------------------
# XDBF

def _xdbf_entries(xdbf):
    magic, _, entry_count, entry_used, free_count, _ = struct.unpack(">4sIIIII", xdbf[:24])
    if magic != b"XDBF":
        raise ValueError("not an XDBF")
    content = 24 + entry_count * 18 + free_count * 8
    entries = {}
    for i in range(entry_used):
        section, eid, offset, size = struct.unpack(">HQII", xdbf[24 + i * 18:42 + i * 18])
        entries[(section, eid)] = xdbf[content + offset:content + offset + size]
    return entries


def _strings(block):
    if not block or block[:4] != b"XSTR":
        return {}
    count = struct.unpack(">H", block[12:14])[0]
    out, p = {}, 14
    for _ in range(count):
        sid, n = struct.unpack(">HH", block[p:p + 4])
        out[sid] = block[p + 4:p + 4 + n].decode("utf-8", "replace")
        p += 4 + n
    return out


def export(xex_path, user_data_dir):
    """Write <user data>/achievements/<title id>-list.json and the icons from default.xex. Returns the
    list's path, or None when the XEX has no achievements."""
    xdbf, title_id = read_xdbf(xex_path)
    if not xdbf:
        return None
    entries = _xdbf_entries(xdbf)
    table = entries.get((1, 0x58414348))  # metadata "XACH"
    if not table or table[:4] != b"XACH":
        return None
    strings = {lang: _strings(entries.get((3, lang))) for lang in LANGUAGE_IDS}
    default = struct.unpack(">I", entries[(1, 0x58535443)][12:16])[0] if (1, 0x58535443) in entries else 1
    if default not in strings:
        strings[default] = _strings(entries.get((3, default)))
    folder = Path(user_data_dir) / "achievements"
    (folder / "icons").mkdir(parents=True, exist_ok=True)
    count = struct.unpack(">H", table[12:14])[0]
    items = []
    for i in range(count):
        aid, label, desc, unach, image, score = struct.unpack(">HHHHIH", table[14 + i * 0x24:14 + i * 0x24 + 14])

        def texts(sid):
            return {str(lang): strings[lang][sid] for lang in LANGUAGE_IDS if strings[lang].get(sid)}

        def plain(sid):
            return strings[default].get(sid) or strings[1].get(sid, "")

        icon = ""
        png = entries.get((2, image))
        if image and png:
            icon = f"icons/{image}.png"
            path = folder / icon
            if not path.exists():
                path.write_bytes(png)
        items.append({"id": aid, "name": plain(label), "description": plain(desc),
                      "locked_description": plain(unach), "gamerscore": score, "icon": icon,
                      "names": texts(label), "descriptions": texts(desc), "locked_descriptions": texts(unach)})
    path = folder / f"{title_id:08X}-list.json"
    path.write_text(json.dumps({"title_id": f"{title_id:08X}", "achievements": items}, ensure_ascii=False, indent=1),
                    encoding="utf-8")
    return path
