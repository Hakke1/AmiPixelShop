#!/usr/bin/env python3
"""Rendert einen TTF-Font in eine Alpha-Glyphentabelle (C-Header).
Aufruf: mkfont.py font.ttf groesse ausgabe.h praefix"""
import sys
from PIL import Image, ImageDraw, ImageFont

path, size, out, pre = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
font = ImageFont.truetype(path, size)
ascent, descent = font.getmetrics()
codes = list(range(32, 127)) + list(range(160, 256))
glyphs, data = [], []
for c in codes:
    ch = chr(c)
    adv = int(round(font.getlength(ch)))
    l, t, r, b = font.getbbox(ch)
    w, h = max(0, r - l), max(0, b - t)
    off = len(data)
    if w and h:
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-l, -t), ch, font=font, fill=255)
        data.extend(im.tobytes())
    glyphs.append((w, h, l, t, adv, off))

with open(out, 'w') as f:
    f.write('/* generiert von tools/mkfont.py aus %s, %d px */\n' % (path.split('/')[-1], size))
    f.write('#define %s_ASCENT %d\n#define %s_HEIGHT %d\n' % (pre.upper(), ascent, pre.upper(), ascent + descent))
    f.write('static const Glyph %s_glyphs[256] = {\n' % pre)
    table = {c: g for c, g in zip(codes, glyphs)}
    for c in range(256):
        w, h, l, t, adv, off = table.get(c, (0, 0, 0, 0, 0, 0))
        f.write('  {%d,%d,%d,%d,%d,%d},\n' % (w, h, l, t, adv, off))
    f.write('};\nstatic const UBYTE %s_alpha[%d] = {\n' % (pre, max(1, len(data))))
    for i in range(0, len(data), 24):
        f.write('  ' + ','.join(str(v) for v in data[i:i+24]) + ',\n')
    f.write('};\n')
print('%s: %d Glyphen, %d Bytes, ascent %d, hoehe %d' % (out, len(codes), len(data), ascent, ascent + descent))
