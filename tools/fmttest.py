#!/usr/bin/env python3
"""Formattest fuer AmiPixelShop: Referenzbilder erzeugen, auf dem Amiga per CONVERT
umwandeln lassen, Ergebnisse pixelgenau vergleichen. Ohne PIL (nur zlib + sips).

  tools/fmttest.py gen   <dir>          Referenzen + Amiga-Skript (fmttest.script) erzeugen
  tools/fmttest.py check <dir> <outdir> Ergebnisse (PNG vom Amiga) vergleichen
"""
import math, os, struct, subprocess, sys, zlib

# ---------------- PNG schreiben/lesen ----------------

def png_write(path, w, h, rows, ctype, depth=8, plte=None, trns=None):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    raw = b"".join(b"\0" + r for r in rows)
    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, 0))
    if plte:
        out += chunk(b"PLTE", plte)
    if trns:
        out += chunk(b"tRNS", trns)
    out += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(out)

def png_read(path):
    """-> (w, h, liste von (r,g,b,a)) fuer 8/16-Bit Gray/RGB/Palette/GA/RGBA, nicht interlaced"""
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path
    p, idat, plte, trns = 8, b"", None, None
    while p < len(d):
        n, t = struct.unpack(">I4s", d[p:p + 8])
        c = d[p + 8:p + 8 + n]
        if t == b"IHDR":
            w, h, depth, ctype, _, _, il = struct.unpack(">IIBBBBB", c)
        elif t == b"PLTE":
            plte = c
        elif t == b"tRNS":
            trns = c
        elif t == b"IDAT":
            idat += c
        p += 12 + n
    assert il == 0, "interlaced"
    ch = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ctype]
    bpp = max(1, ch * depth // 8)
    stride = (w * ch * depth + 7) // 8
    raw = zlib.decompress(idat)
    prev = bytearray(stride)
    px = []
    for y in range(h):
        f = raw[y * (stride + 1)]
        r = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = r[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: r[i] = (r[i] + a) & 255
            elif f == 2: r[i] = (r[i] + b) & 255
            elif f == 3: r[i] = (r[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                r[i] = (r[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        prev = r
        for x in range(w):
            if depth == 16:
                v = [r[(x * ch + k) * 2] for k in range(ch)]
            elif depth == 8:
                v = list(r[x * ch:(x + 1) * ch])
            else:                       # 1/2/4 Bit (nur Gray/Palette)
                bit = x * depth
                v = [(r[bit >> 3] >> (8 - depth - (bit & 7))) & ((1 << depth) - 1)]
            if ctype == 3:
                i = v[0]
                a = trns[i] if trns and i < len(trns) else 255
                px.append((plte[i * 3], plte[i * 3 + 1], plte[i * 3 + 2], a))
            elif ctype == 0:
                g = v[0] * 255 // ((1 << depth) - 1) if depth < 8 else v[0]
                px.append((g, g, g, 255))
            elif ctype == 4:
                px.append((v[0], v[0], v[0], v[1]))
            elif ctype == 2:
                px.append((v[0], v[1], v[2], 255))
            else:
                px.append(tuple(v))
    return w, h, px

# ---------------- weitere Formate ----------------

def bmp_write(path, w, h, px):
    rs = (w * 3 + 3) & ~3
    body = b"".join(bytes(b for (r, g, bb, a) in px[y * w:(y + 1) * w] for b in (bb, g, r)) + b"\0" * (rs - w * 3)
                    for y in range(h - 1, -1, -1))
    hdr = b"BM" + struct.pack("<IHHI", 54 + len(body), 0, 0, 54) + struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(body), 2835, 2835, 0, 0)
    open(path, "wb").write(hdr + body)

def tga_write(path, w, h, px):
    hdr = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, w, h, 32, 8)
    body = b"".join(bytes(b for (r, g, bb, a) in px[y * w:(y + 1) * w] for b in (bb, g, r, a)) for y in range(h - 1, -1, -1))
    open(path, "wb").write(hdr + body)

def byterun1(row):
    out, i = bytearray(), 0
    while i < len(row):
        j = i
        while j + 1 < len(row) and row[j + 1] == row[i] and j - i < 127:
            j += 1
        if j - i >= 2:
            out += bytes([257 - (j - i + 1), row[i]])
            i = j + 1
        else:
            k = i
            while k < len(row) and k - i < 128 and not (k + 2 < len(row) and row[k] == row[k + 1] == row[k + 2]):
                k += 1
            out += bytes([k - i - 1]) + row[i:k]
            i = k
    return bytes(out)

def ilbm_write(path, w, h, px, deep, comp):
    """deep: 24-Bit-ILBM (planes=24), sonst Palette aus den Farben (<=256)"""
    rb = ((w + 15) >> 4) << 1
    if deep:
        planes, cmap, idx = 24, b"", None
    else:
        cols = sorted(set((r, g, b) for (r, g, b, a) in px))
        assert len(cols) <= 256
        planes = max(1, math.ceil(math.log2(len(cols)))) if len(cols) > 1 else 1
        lut = {c: i for i, c in enumerate(cols)}
        idx = [lut[(r, g, b)] for (r, g, b, a) in px]
        cmap = b"".join(bytes(c) for c in cols)
    body = bytearray()
    for y in range(h):
        for p in range(planes):
            row = bytearray(rb)
            for x in range(w):
                if deep:
                    r, g, b, a = px[y * w + x]
                    v = (r, g, b)[p // 8] >> (p % 8) & 1
                else:
                    v = idx[y * w + x] >> p & 1
                if v:
                    row[x >> 3] |= 0x80 >> (x & 7)
            body += byterun1(bytes(row)) if comp else row
    bmhd = struct.pack(">HHhhBBBBHBBhh", w, h, 0, 0, planes, 0, 1 if comp else 0, 0, 0, 10, 11, w, h)
    def ck(t, d):
        return t + struct.pack(">I", len(d)) + d + (b"\0" if len(d) & 1 else b"")
    form = b"ILBM" + ck(b"BMHD", bmhd) + (ck(b"CMAP", cmap) if cmap else b"") + ck(b"BODY", bytes(body))
    open(path, "wb").write(b"FORM" + struct.pack(">I", len(form)) + form)

def sips(src, fmt, dst):
    subprocess.run(["sips", "-s", "format", fmt, src, "--out", dst], check=True, capture_output=True)

# ---------------- Testbilder ----------------

def img_rgb(w, h):
    return [(x * 255 // (w - 1), y * 255 // (h - 1), (x * 7 + y * 13) & 255, 255) for y in range(h) for x in range(w)]

def img_rgba(w, h):
    return [(x * 255 // (w - 1), (x ^ y) & 255, y * 255 // (h - 1), (x + 2 * y) % 256) for y in range(h) for x in range(w)]

def img_pal(w, h, n):
    cols = [((i * 37) & 255, (i * 91) & 255, (i * 151) & 255) for i in range(n)]
    return [cols[((x // 7) + (y // 5) * 13) % n] + (255,) for y in range(h) for x in range(w)]

def rows8(px, w, h, ch):
    return [bytes(v for p in px[y * w:(y + 1) * w] for v in p[:ch]) for y in range(h)]

# Testfaelle: name -> (eingabedatei, referenz-png, toleranz, alpha-pruefen)
def gen(d):
    os.makedirs(d, exist_ok=True)
    cases = []
    W, H = 301, 203                     # ungerade, keine Kachelvielfachen
    rgb, rgba = img_rgb(W, H), img_rgba(W, H)
    png_write(f"{d}/rgb.png", W, H, rows8(rgb, W, H, 3), 2)
    png_write(f"{d}/rgba.png", W, H, rows8(rgba, W, H, 4), 6)
    cases += [("png_rgb", "rgb.png", "rgb.png", 0), ("png_rgba", "rgba.png", "rgba.png", 0)]
    # Graustufen 8 Bit, 16-Bit RGB, Palette mit tRNS, 1-Bit
    gray = [(v, v, v, 255) for v in ((x + y) & 255 for y in range(H) for x in range(W))]
    png_write(f"{d}/gray.png", W, H, [bytes(p[0] for p in gray[y * W:(y + 1) * W]) for y in range(H)], 0)
    png_write(f"{d}/gray_ref.png", W, H, rows8(gray, W, H, 3), 2)
    cases.append(("png_gray", "gray.png", "gray_ref.png", 0))
    png_write(f"{d}/rgb16.png", W, H, [bytes(b for p in rgb[y * W:(y + 1) * W] for v in p[:3] for b in (v, (v * 3) & 255)) for y in range(H)], 2, 16)
    cases.append(("png_16bit", "rgb16.png", "rgb.png", 1))
    pal = img_pal(W, H, 40)
    cols = sorted(set(p[:3] for p in pal))
    lut = {c: i for i, c in enumerate(cols)}
    trns = bytes(0 if i == 3 else 128 if i == 5 else 255 for i in range(len(cols)))
    png_write(f"{d}/pal.png", W, H, [bytes(lut[p[:3]] for p in pal[y * W:(y + 1) * W]) for y in range(H)], 3,
              plte=b"".join(bytes(c) for c in cols), trns=trns)
    palref = [p[:3] + (trns[lut[p[:3]]],) for p in pal]
    png_write(f"{d}/pal_ref.png", W, H, rows8(palref, W, H, 4), 6)
    cases.append(("png_palette_trns", "pal.png", "pal_ref.png", 0))
    bw = [(255, 255, 255, 255) if (x // 3 + y // 3) & 1 else (0, 0, 0, 255) for y in range(H) for x in range(W)]
    png_write(f"{d}/bw1.png", W, H, [bytes(sum((0x80 >> k) for k in range(8) if x0 + k < W and bw[y * W + x0 + k][0]) for x0 in range(0, W, 8)) for y in range(H)], 0, 1)
    png_write(f"{d}/bw_ref.png", W, H, rows8(bw, W, H, 3), 2)
    cases.append(("png_1bit", "bw1.png", "bw_ref.png", 0))
    # Grenzfaelle
    png_write(f"{d}/one.png", 1, 1, [bytes((10, 200, 30, 255))], 6)
    cases.append(("png_1x1", "one.png", "one.png", 0))
    tall = img_rgb(3, 999)
    png_write(f"{d}/tall.png", 3, 999, rows8(tall, 3, 999, 3), 2)
    cases.append(("png_3x999", "tall.png", "tall.png", 0))
    BW, BH = 2049, 1537
    big = [((x * 3) & 255, (y * 5) & 255, (x + y) & 255, 255) for y in range(BH) for x in range(BW)]
    png_write(f"{d}/big.png", BW, BH, rows8(big, BW, BH, 3), 2)
    cases.append(("png_2049x1537", "big.png", "big.png", 0))
    # BMP, TGA (mit Alpha)
    bmp_write(f"{d}/rgb.bmp", W, H, rgb)
    cases.append(("bmp_24", "rgb.bmp", "rgb.png", 0))
    tga_write(f"{d}/rgba.tga", W, H, rgba)
    cases.append(("tga_32", "rgba.tga", "rgba.png", 0))
    # GIF/JPEG/PSD/TIFF ueber sips; Referenz = sips zurueck nach PNG
    for fmt, ext, tol in (("gif", "gif", 0), ("jpeg", "jpg", 6), ("psd", "psd", 0), ("tiff", "tif", 0)):
        sips(f"{d}/rgb.png", fmt, f"{d}/sips.{ext}")
        sips(f"{d}/sips.{ext}", "png", f"{d}/sips_{ext}_ref.png")
        cases.append((f"{ext}_sips", f"sips.{ext}", f"sips_{ext}_ref.png", tol))
    sips(f"{d}/rgba.png", "gif", f"{d}/sipsa.gif")
    sips(f"{d}/sipsa.gif", "png", f"{d}/sipsa_gif_ref.png")
    cases.append(("gif_transparent", "sipsa.gif", "sipsa_gif_ref.png", 0))
    # IFF ILBM: 24 Bit roh/gepackt, Palette gepackt
    ilbm_write(f"{d}/deep.iff", W, H, rgb, True, False)
    cases.append(("ilbm_24bit_raw", "deep.iff", "rgb.png", 0))
    ilbm_write(f"{d}/deepc.iff", W, H, rgb, True, True)
    cases.append(("ilbm_24bit_byterun", "deepc.iff", "rgb.png", 0))
    pal_o = img_pal(W, H, 40)
    png_write(f"{d}/pal40.png", W, H, rows8(pal_o, W, H, 3), 2)
    ilbm_write(f"{d}/pal.iff", W, H, pal_o, False, True)
    cases.append(("ilbm_6planes", "pal.iff", "pal40.png", 0))
    # Rundlaeufe: Eingabe -> Zwischenformat -> PNG
    rts = [("rt_aps_rgba", "rgba.png", "aps", "rgba.png", 0),
           ("rt_aps_big", "big.png", "aps", "big.png", 0),
           ("rt_iff_40colors", "pal40.png", "iff", "pal40.png", 0),
           ("rt_iff_alpha", "pal.png", "iff", "pal_ref.png", -1),
           ("rt_jpg", "rgb.png", "jpg", "rgb.png", 12)]
    # Amiga-Skript
    sc = ["; erzeugt von tools/fmttest.py", "FailAt 21", "Cd Work:qt/ft", "MakeDir out"]
    for name, inp, ref, tol in cases:
        sc.append(f"Echo \"== {name}\"")
        sc.append(f"Work:AmiPixelShop/AmiPixelShop CONVERT {inp} out/{name}.png")
    for name, inp, mid, ref, tol in rts:
        sc.append(f"Echo \"== {name}\"")
        sc.append(f"Work:AmiPixelShop/AmiPixelShop CONVERT {inp} out/{name}.{mid}")
        sc.append(f"Work:AmiPixelShop/AmiPixelShop CONVERT out/{name}.{mid} out/{name}.png")
    open(f"{d}/fmttest.script", "w").write("\n".join(sc) + "\n")
    with open(f"{d}/cases.txt", "w") as f:
        for name, inp, ref, tol in cases:
            f.write(f"{name} {ref} {tol}\n")
        for name, inp, mid, ref, tol in rts:
            f.write(f"{name} {ref} {tol}\n")
    print(f"{len(cases) + len(rts)} cases in {d}")

def check(d, out):
    bad = 0
    for line in open(f"{d}/cases.txt"):
        name, ref, tol = line.split()
        tol = int(tol)
        p = f"{out}/{name}.png"
        if not os.path.exists(p):
            print(f"FAIL {name:22s} no output"); bad += 1; continue
        w1, h1, a = png_read(f"{d}/{ref}")
        w2, h2, b = png_read(p)
        if (w1, h1) != (w2, h2):
            print(f"FAIL {name:22s} size {w2}x{h2}, expected {w1}x{h1}"); bad += 1; continue
        if tol < 0:                       # nur Transparenz pruefen (IFF: Farben quantisiert)
            wrong = sum(1 for p1, p2 in zip(a, b) if (p1[3] == 0) != (p2[3] == 0))
            print(f"{'ok  ' if wrong == 0 else 'FAIL'} {name:22s} transparent pixels differ: {wrong}")
            bad += wrong > 0
            continue
        mx, sm, n, aerr, first = 0, 0, 0, 0, None
        for i, (p1, p2) in enumerate(zip(a, b)):
            if abs(p1[3] - p2[3]) > tol:
                aerr += 1
                first = first or (i % w1, i // w1, p1, p2)
            if p1[3] == 0:
                continue
            t = tol + (255 // p1[3] if p1[3] < 255 else 0)   # Rundung bei Premultiply
            e = max(abs(p1[k] - p2[k]) for k in range(3))
            mx, sm, n = max(mx, e), sm + e, n + 1
            if e > t and first is None:
                first = (i % w1, i // w1, p1, p2)
            if e > t:
                aerr += 1
        ok = aerr == 0
        bad += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {name:22s} max {mx:3d} mean {sm / max(n, 1):5.2f} bad px {aerr}"
              + (f"  first at {first[0]},{first[1]}: want {first[2]} got {first[3]}" if first else ""))
    print("ALL OK" if not bad else f"{bad} FAILED")

if __name__ == "__main__":
    {"gen": lambda: gen(sys.argv[2]), "check": lambda: check(sys.argv[2], sys.argv[3])}[sys.argv[1]]()
