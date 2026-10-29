#!/usr/bin/env python3
"""Formattest fuer AmiPixelShop: Referenzbilder erzeugen, auf dem Amiga per CONVERT
umwandeln lassen, Ergebnisse pixelgenau vergleichen. Ohne PIL (nur zlib + sips).

  tools/fmttest.py gen   <dir>          Referenzen + Amiga-Skripte (get.script, fmttest.script) erzeugen
  tools/fmttest.py check <dir> <outdir> Ergebnisse (PNG vom Amiga) vergleichen
  tools/fmttest.py recv  <outdir>       Ergebnisse vom Amiga annehmen (Port 8069)

Ablauf: make serve (Port 8068), fmttest.py recv <outdir> (Port 8069), auf dem Amiga
"wget -q -O RAM:g http://HOST:8068/ft/get.script", "Execute RAM:g",
"Execute Work:qt/ft/fmttest.script", dann check.
"""
import math, os, struct, subprocess, sys, zlib

HOST = "192.168.178.119"     # Mac: Port 8068 liefert build/, 8069 nimmt Ergebnisse an
APS = "Work:qt/ft/AmiPixelShop"   # frischer Build aus build/, nicht die installierte Version

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

def lzw_tiff(data):
    """TIFF-LZW wie libtiff: MSB zuerst, Breitenwechsel nach Eintrag 511, Clear bei 4094"""
    out, acc, nb, width = bytearray(), 0, 0, 9
    def put(code):
        nonlocal acc, nb
        acc = (acc << width | code) & 0xFFFFFFFF; nb += width
        while nb >= 8:
            out.append(acc >> (nb - 8) & 255); nb -= 8
    table, nxt = {}, 258
    put(256)
    w = None
    for b in data:
        if w is None:
            w = b; continue
        k = (w, b)
        c = table.get(k)
        if c is not None:
            w = c; continue
        put(w)
        table[k] = nxt; nxt += 1
        if nxt > 4094:
            put(256); table = {}; nxt = 258; width = 9
        elif nxt > (1 << width) - 1:
            width += 1
        w = b
    if w is not None:
        put(w)
    put(257)
    if nb:
        out.append(acc << (8 - nb) & 255)
    return bytes(out)

def packbits(row):
    return byterun1(row)

def tiff_write(path, w, h, rows, spp, bps, photo, comp=1, rps=None, le=False, pred=1, extra=None, cmap=None):
    """rows: rohe Zeilenbytes (fertig gepackt nach bps); comp 1/5/8/32773"""
    E = "<" if le else ">"
    rps = rps or h
    if pred == 2:
        rows2 = []
        for r in rows:
            r = bytearray(r)
            if bps == 8:
                for x in range(len(r) - 1, spp - 1, -1):
                    r[x] = (r[x] - r[x - spp]) & 255
            rows2.append(bytes(r))
        rows = rows2
    strips = []
    for y in range(0, h, rps):
        raw = b"".join(rows[y:y + rps])
        if comp == 5: raw = lzw_tiff(raw)
        elif comp == 8: raw = zlib.compress(raw)
        elif comp == 32773: raw = b"".join(packbits(r) for r in rows[y:y + rps])
        strips.append(raw)
    n = len(strips)
    tags = []
    def tag(t, ty, vals):
        tags.append((t, ty, vals))
    tag(256, 4, [w]); tag(257, 4, [h]); tag(258, 3, [bps] * spp); tag(259, 3, [comp]); tag(262, 3, [photo])
    tag(273, 4, [0] * n); tag(277, 3, [spp]); tag(278, 4, [rps]); tag(279, 4, [len(x) for x in strips])
    tag(284, 3, [1])
    if pred != 1: tag(317, 3, [pred])
    if cmap: tag(320, 3, cmap)
    if extra is not None: tag(338, 3, [extra])
    tags.sort()
    ifd_off = 8
    data_off = ifd_off + 2 + 12 * len(tags) + 4
    blobs, entries = bytearray(), []
    # Werte > 4 Byte nach dem IFD, Streifen danach
    for t, ty, vals in tags:
        sz = 2 if ty == 3 else 4
        raw = b"".join(struct.pack(E + ("H" if ty == 3 else "I"), v) for v in vals)
        if len(raw) <= 4:
            entries.append([t, ty, len(vals), raw.ljust(4, b"\0"), None])
        else:
            entries.append([t, ty, len(vals), None, len(blobs)])
            blobs += raw + (b"\0" if len(raw) & 1 else b"")
    strip_base = data_off + len(blobs)
    offs, o = [], strip_base
    for x in strips:
        offs.append(o); o += len(x)
    # Streifen-Offsets eintragen
    for e in entries:
        if e[0] == 273:
            raw = b"".join(struct.pack(E + "I", v) for v in offs)
            if len(raw) <= 4: e[3] = raw
            else: blobs[e[4]:e[4] + len(raw)] = raw
    ifd = struct.pack(E + "H", len(entries))
    for t, ty, cnt, inl, bo in entries:
        ifd += struct.pack(E + "HHI", t, ty, cnt) + (inl if inl is not None else struct.pack(E + "I", data_off + bo))
    ifd += struct.pack(E + "I", 0)
    hdr = (b"II*\0" if le else b"MM\0*") + struct.pack(E + "I", ifd_off)
    open(path, "wb").write(hdr + ifd + bytes(blobs) + b"".join(strips))

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
    d = os.path.abspath(d)
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
    # JPEG-Referenz: dieselbe stb-Version nativ (Decoder unterscheiden sich sonst an Farbkanten)
    stb = f"{d}/stbref"
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "stbref.c")
    inc = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "third_party", "stb")
    subprocess.run(["cc", "-O2", "-w", "-I" + inc, "-o", stb, src, "-lm"], check=True)
    for fmt, ext in (("gif", "gif"), ("jpeg", "jpg"), ("psd", "psd"), ("tiff", "tif")):
        sips(f"{d}/rgb.png", fmt, f"{d}/sips.{ext}")
        if ext == "jpg":
            subprocess.run([stb, "dec", f"{d}/sips.jpg", f"{d}/sips_jpg_ref.png"], check=True, cwd=d)
        else:
            sips(f"{d}/sips.{ext}", "png", f"{d}/sips_{ext}_ref.png")
        cases.append((f"{ext}_sips", f"sips.{ext}", f"sips_{ext}_ref.png", 0))
    sips(f"{d}/rgba.png", "gif", f"{d}/sipsa.gif")
    sips(f"{d}/sipsa.gif", "png", f"{d}/sipsa_gif_ref.png")
    cases.append(("gif_transparent", "sipsa.gif", "sipsa_gif_ref.png", 0))
    # TIFF: eigener Lader (akTIFF.datatype setzt Streifen falsch zusammen)
    r3, r4 = rows8(rgb, W, H, 3), rows8(rgba, W, H, 4)
    tiff_write(f"{d}/t_mm_none.tif", W, H, r3, 3, 8, 2, rps=50)
    cases.append(("tif_mm_none_5strips", "t_mm_none.tif", "rgb.png", 0))
    tiff_write(f"{d}/t_ii_lzw.tif", W, H, r3, 3, 8, 2, comp=5, rps=16, le=True, pred=2)
    cases.append(("tif_ii_lzw_pred", "t_ii_lzw.tif", "rgb.png", 0))
    big3 = rows8(big, BW, BH, 3)
    tiff_write(f"{d}/t_big_lzw.tif", BW, BH, big3, 3, 8, 2, comp=5, rps=BH)
    cases.append(("tif_big_lzw_1strip", "t_big_lzw.tif", "big.png", 0))
    tiff_write(f"{d}/t_rgba_defl.tif", W, H, r4, 4, 8, 2, comp=8, rps=64, extra=2)
    cases.append(("tif_rgba_deflate", "t_rgba_defl.tif", "rgba.png", 0))
    tiff_write(f"{d}/t_gray_pb.tif", W, H, [bytes(p[0] for p in gray[y * W:(y + 1) * W]) for y in range(H)], 1, 8, 1, comp=32773, rps=30)
    cases.append(("tif_gray_packbits", "t_gray_pb.tif", "gray_ref.png", 0))
    tiff_write(f"{d}/t_rgb16.tif", W, H, [bytes(b for p in rgb[y * W:(y + 1) * W] for v in p[:3] for b in (v, 77)) for y in range(H)], 3, 16, 2, le=False, rps=40)
    cases.append(("tif_rgb16_mm", "t_rgb16.tif", "rgb.png", 0))
    tiff_write(f"{d}/t_rgb16le.tif", W, H, [bytes(b for p in rgb[y * W:(y + 1) * W] for v in p[:3] for b in (77, v)) for y in range(H)], 3, 16, 2, le=True, rps=40)
    cases.append(("tif_rgb16_ii", "t_rgb16le.tif", "rgb.png", 0))
    cm = [c[0] * 257 for c in cols] + [0] * (256 - len(cols)) + [c[1] * 257 for c in cols] + [0] * (256 - len(cols)) + [c[2] * 257 for c in cols] + [0] * (256 - len(cols))
    tiff_write(f"{d}/t_pal.tif", W, H, [bytes(lut[p[:3]] for p in pal[y * W:(y + 1) * W]) for y in range(H)], 1, 8, 3, comp=5, rps=20, cmap=cm)
    png_write(f"{d}/pal_opaque_ref.png", W, H, rows8(pal, W, H, 3), 2)
    cases.append(("tif_palette_lzw", "t_pal.tif", "pal_opaque_ref.png", 0))
    tiff_write(f"{d}/t_bw.tif", W, H, [bytes(sum((0x80 >> k) for k in range(8) if x0 + k < W and not bw[y * W + x0 + k][0]) for x0 in range(0, W, 8)) for y in range(H)], 1, 1, 0, rps=100)
    cases.append(("tif_1bit_whiteiszero", "t_bw.tif", "bw_ref.png", 0))
    for opt in ("lzw", "packbits"):
        subprocess.run(["sips", "-s", "format", "tiff", "-s", "formatOptions", opt, f"{d}/rgb.png", "--out", f"{d}/s_{opt}.tif"], check=True, capture_output=True)
        sips(f"{d}/s_{opt}.tif", "png", f"{d}/s_{opt}_ref.png")
        cases.append((f"tif_sips_{opt}", f"s_{opt}.tif", f"s_{opt}_ref.png", 0))
    subprocess.run(["sips", "-s", "format", "tiff", "-s", "formatOptions", "lzw", f"{d}/rgba.png", "--out", f"{d}/s_rgba.tif"], check=True, capture_output=True)
    sips(f"{d}/s_rgba.tif", "png", f"{d}/s_rgba_ref.png")
    cases.append(("tif_sips_rgba_lzw", "s_rgba.tif", "s_rgba_ref.png", 1))
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
           ("rt_jpg", "rgb.png", "jpg", "stb_rt_ref.png", 0)]
    subprocess.run([stb, "rt", f"{d}/rgb.png", f"{d}/stb_rt_ref.png"], check=True, cwd=d)
    # Amiga-Skript
    sc = ["; erzeugt von tools/fmttest.py", "FailAt 21", "Cd Work:qt/ft", "MakeDir out"]
    for name, inp, ref, tol in cases:
        sc.append(f"Echo \"== {name}\"")
        sc.append(f"{APS} CONVERT {inp} out/{name}.png")
    for name, inp, mid, ref, tol in rts:
        sc.append(f"Echo \"== {name}\"")
        sc.append(f"{APS} CONVERT {inp} out/{name}.{mid}")
        sc.append(f"{APS} CONVERT out/{name}.{mid} out/{name}.png")
    for name in [c[0] for c in cases] + [r[0] for r in rts]:
        sc.append(f"wget -q --post-file=out/{name}.png -O NIL: http://{HOST}:8069/{name}.png")
    open(f"{d}/fmttest.script", "w").write("\n".join(sc) + "\n")
    files = sorted(f for f in os.listdir(d) if not f.endswith("_ref.png") and
                   f not in ("cases.txt", "get.script", "stbref", "stbref_tmp.jpg"))
    open(f"{d}/get.script", "w").write("MakeDir Work:qt/ft\n" + "".join(
        f"wget -q -O Work:qt/ft/{f} http://{HOST}:8068/ft/{f}\n" for f in files) +
        f"wget -q -O {APS} http://{HOST}:8068/AmiPixelShop\nProtect {APS} +e\n")
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

def recv(out):
    """Empfaenger fuer "wget --post-file" vom Amiga: speichert unter <out>/<name>"""
    import http.server
    os.makedirs(out, exist_ok=True)
    class H(http.server.BaseHTTPRequestHandler):
        def do_POST(self):
            data = self.rfile.read(int(self.headers.get("Content-Length", 0)))
            name = os.path.basename(self.path)
            if name:
                open(os.path.join(out, name), "wb").write(data)
            self.send_response(200); self.send_header("Content-Length", "2"); self.end_headers(); self.wfile.write(b"ok")
        def log_message(self, *a):
            pass
    http.server.HTTPServer(("", 8069), H).serve_forever()

if __name__ == "__main__":
    {"gen": lambda: gen(sys.argv[2]), "check": lambda: check(sys.argv[2], sys.argv[3]),
     "recv": lambda: recv(sys.argv[2])}[sys.argv[1]]()
