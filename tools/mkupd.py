#!/usr/bin/env python3
"""Update-Paket AmiPixelShop.upd fuer den eingebauten Updater (src/update.c) bauen.

Aufruf: tools/mkupd.py <ausgabe.upd>   (im Projektverzeichnis, nach make)
Enthaelt Programm, Guide, Bilder, LICENSE und Readme - nicht das Icon, damit
eigene Icon-Positionen und Tooltypes erhalten bleiben.
"""
import glob, os, sys, zlib

files = [("AmiPixelShop", "build/AmiPixelShop"), ("Guide.html", "docs/Guide.html"),
         ("LICENSE", "LICENSE"), ("AmiPixelShop.readme", "AmiPixelShop.readme")]
files += [("images/" + os.path.basename(p), p) for p in sorted(glob.glob("docs/images/*.jpg"))]
files += [("ComfyUI/" + os.path.basename(p), p) for p in sorted(glob.glob("dist/ComfyUI/*"))]

out = bytearray(b"APSUPD1\n")
for name, src in files:
    data = open(src, "rb").read()
    comp = zlib.compress(data, 9)
    out += b"F %s %d %d %08x\n" % (name.encode(), len(data), len(comp), zlib.crc32(data) & 0xFFFFFFFF)
    out += comp
out += b"END\n"
open(sys.argv[1], "wb").write(out)
print("%s: %d files, %d bytes" % (sys.argv[1], len(files), len(out)))
