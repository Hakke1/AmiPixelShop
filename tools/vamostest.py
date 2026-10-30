#!/usr/bin/env python3
"""Rauchtest ohne Amiga: AmiPixelShop LAYERTEST unter vamos (amitools) auf einem
emulierten 68040 - Ebenen speichern, laden, zusammenfuegen.

Aufruf: tools/vamostest.py build/AmiPixelShop <arbeitsverzeichnis>
Braucht: pip install amitools==0.8.1 machine68k==0.3.0

Prueft, dass die PNGs gueltig sind (Chunk-CRCs, zlib-Pruefsumme) und das Bild nach
Speichern/Laden und nach dem Zusammenfuegen pixelgleich ist. Die Grafik-Bibliotheken
werden nur vorgetaeuscht; FILTERTEST geht nicht (vamos kennt FSIN nicht).
"""
import os, struct, subprocess, sys, zlib

LIBS = "+".join([
    "intuition.library=mode:fake,version:40",
    "graphics.library=mode:fake,version:40",
    "Picasso96API.library=mode:fake,version:2,num_fake_funcs:200",
    "keymap.library=mode:fake,version:40",
    "datatypes.library=mode:fake,version:45",
    "iffparse.library=mode:fake,version:40",
    "asl.library=mode:fake,version:40",
])


def pixels(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path + ": no PNG"
    i, idat = 8, b""
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        assert zlib.crc32(t + c) & 0xFFFFFFFF == struct.unpack(">I", d[i + 8 + n:i + 12 + n])[0], \
            "%s: bad CRC in %s" % (path, t)
        if t == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    raw = zlib.decompress(idat)         # prueft auch die Adler-32-Summe
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    s, out, prev = w * bpp, bytearray(), bytearray(w * bpp)
    for y in range(h):
        f, r = raw[y * (s + 1)], bytearray(raw[y * (s + 1) + 1:(y + 1) * (s + 1)])
        for x in range(s):
            a, b = r[x - bpp] if x >= bpp else 0, prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                r[x] = (r[x] + a) & 255
            elif f == 2:
                r[x] = (r[x] + b) & 255
            elif f == 3:
                r[x] = (r[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                r[x] = (r[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += r
        prev = r
    return (w, h), bytes(out)


def main():
    exe, work = sys.argv[1], os.path.abspath(sys.argv[2])
    os.makedirs(os.path.join(work, "out"), exist_ok=True)
    os.makedirs(os.path.join(work, "vols"), exist_ok=True)
    log = subprocess.run(
        ["vamos", "--vols-base-dir", os.path.join(work, "vols"), "-C", "68040", "-H", "disable",
         "-m", "262144", "-s", "512", "-O", LIBS, os.path.abspath(exe), "LAYERTEST", "out"],
        cwd=work, capture_output=True, text=True, timeout=1200)
    print(log.stdout)
    if "1 layers after flatten" not in log.stdout:
        print(log.stderr[-3000:])
        sys.exit("LAYERTEST did not finish")
    out = os.path.join(work, "out")
    before = pixels(os.path.join(out, "before.png"))
    for name in ("after.png", "flat.png"):
        assert pixels(os.path.join(out, name)) == before, name + " differs from before.png"
    print("OK: %dx%d, PNGs valid, save/load and flatten pixel-identical" % before[0])


main()
