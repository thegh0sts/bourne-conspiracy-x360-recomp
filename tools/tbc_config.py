"""
Read and edit The Bourne Conspiracy's packed engine config (Coalesced.ini).

The Xbox 360 build stores its .ini files in BourneGame\\Config\\Xenon\\Cooked\\Coalesced.ini,
XOR-scrambled with a 61-byte key that runs continuously through the file (each string's
terminating NUL is stored as-is and does not advance the key). BourneGame\\Xbox360TOC.txt
lists the file's size and MD5, so both are updated after an edit. Backups (.bak) of both
files are made on the first edit and never overwritten.

Usage:
  python tbc_config.py <game folder> dump [out_dir]
  python tbc_config.py <game folder> get  <file.ini> <Section> <Key>
  python tbc_config.py <game folder> set  <file.ini> <Section> <Key> <Value> [...more Section Key Value]
  python tbc_config.py <game folder> restore

Example (raise the engine's frame rate cap):
  python tbc_config.py "C:\\...\\tbc-game" set Xe-BourneEngine.ini Engine.GameEngine MaxSmoothedFrameRate 144
"""

import hashlib
import shutil
import struct
import sys
from pathlib import Path

KEY = b"as;dwepo2345098]qw]{}p2039458pseasdfzcvvp;aseiurwefsdcfszdcvn"
assert len(KEY) == 61

CONFIG_REL = Path("BourneGame") / "Config" / "Xenon" / "Cooked" / "Coalesced.ini"
TOC_REL = Path("BourneGame") / "Xbox360TOC.txt"
TOC_NAME = r"..\BourneGame\Config\Xenon\Cooked\Coalesced.ini"


def decode(data: bytes):
    """Return [(name, body), ...] in file order."""
    count = struct.unpack(">I", data[:4])[0]
    pos, idx, strings = 4, 0, []
    for _ in range(count):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        raw = data[pos + 4:pos + 4 + n]
        pos += 4 + n
        if raw[-1:] != b"\0":
            raise ValueError("unexpected string terminator")
        strings.append(bytes(b ^ KEY[(idx + j) % 61] for j, b in enumerate(raw[:-1])).decode("latin-1"))
        idx += n - 1
    if pos != len(data) or count % 2:
        raise ValueError("unexpected Coalesced.ini layout")
    return [(strings[i], strings[i + 1]) for i in range(0, count, 2)]


def encode(files) -> bytes:
    out, idx = [struct.pack(">I", len(files) * 2)], 0
    for name, body in files:
        for s in (name, body):
            b = s.encode("latin-1")
            out.append(struct.pack(">I", len(b) + 1))
            out.append(bytes(c ^ KEY[(idx + j) % 61] for j, c in enumerate(b)) + b"\0")
            idx += len(b)
    return b"".join(out)


def find_file(files, short):
    for i, (name, _) in enumerate(files):
        if name.split("\\")[-1].lower() == short.lower():
            return i
    raise SystemExit(f"No {short} in Coalesced.ini. Available: " + ", ".join(n.split('\\')[-1] for n, _ in files))


def get_value(body, section, key):
    cur = None
    for line in body.split("\r\n"):
        if line.startswith("[") and line.endswith("]"):
            cur = line[1:-1]
        elif cur and cur.lower() == section.lower() and line.split("=", 1)[0].lower() == key.lower():
            return line.split("=", 1)[1]
    return None


def set_value(body, section, key, value):
    lines = body.split("\r\n")
    cur, sec_end, done = None, None, False
    for i, line in enumerate(lines):
        if line.startswith("[") and line.endswith("]"):
            if cur and cur.lower() == section.lower():
                sec_end = i
            cur = line[1:-1]
        elif cur and cur.lower() == section.lower():
            if line.split("=", 1)[0].lower() == key.lower():
                lines[i] = f"{key}={value}"
                done = True
                break
    if not done:
        if sec_end is None and cur and cur.lower() == section.lower():
            sec_end = len(lines)
        if sec_end is None:
            raise SystemExit(f"Section [{section}] not found")
        while sec_end > 0 and lines[sec_end - 1] == "":
            sec_end -= 1
        lines.insert(sec_end, f"{key}={value}")
    return "\r\n".join(lines)


def write(game: Path, files):
    cfg, toc = game / CONFIG_REL, game / TOC_REL
    for p in (cfg, toc):
        bak = p.with_name(p.name + ".bak")
        if not bak.exists():
            shutil.copy2(p, bak)
    data = encode(files)
    cfg.write_bytes(data)
    md5 = hashlib.md5(data).hexdigest()
    lines = toc.read_bytes().decode("latin-1").split("\n")
    for i, line in enumerate(lines):
        parts = line.rstrip("\r").split(" ")
        if len(parts) >= 5 and parts[3].lower() == TOC_NAME.lower():
            parts[0], parts[4] = str(len(data)), md5
            lines[i] = " ".join(parts) + ("\r" if line.endswith("\r") else "")
            break
    else:
        raise SystemExit("Coalesced.ini not listed in Xbox360TOC.txt")
    toc.write_bytes("\n".join(lines).encode("latin-1"))
    print(f"Wrote {cfg} ({len(data)} bytes, md5 {md5}) and updated {toc.name}")


def main(argv):
    if len(argv) < 2:
        raise SystemExit(__doc__)
    game, cmd = Path(argv[0]), argv[1]
    cfg = game / CONFIG_REL
    if cmd == "restore":
        for p in (cfg, game / TOC_REL):
            bak = p.with_name(p.name + ".bak")
            if bak.exists():
                shutil.copy2(bak, p)
                print("Restored", p)
        return
    files = decode(cfg.read_bytes())
    if cmd == "dump":
        out = Path(argv[2]) if len(argv) > 2 else Path("coalesced_dump")
        out.mkdir(parents=True, exist_ok=True)
        for name, body in files:
            (out / name.split("\\")[-1]).write_text(body, encoding="latin-1", newline="")
        print(f"Wrote {len(files)} files to {out}")
    elif cmd == "get":
        i = find_file(files, argv[2])
        print(get_value(files[i][1], argv[3], argv[4]))
    elif cmd == "set":
        i = find_file(files, argv[2])
        rest = argv[3:]
        if not rest or len(rest) % 3:
            raise SystemExit("set needs Section Key Value triples")
        name, body = files[i]
        for s, k, v in zip(rest[0::3], rest[1::3], rest[2::3]):
            old = get_value(body, s, k)
            body = set_value(body, s, k, v)
            print(f"[{s}] {k}: {old} -> {v}")
        files[i] = (name, body)
        write(game, files)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main(sys.argv[1:])
