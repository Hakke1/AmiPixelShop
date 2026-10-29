#!/usr/bin/env python3
"""Ergebnisse von "AmiPixelShop LAYERTEST <dir>" pruefen: before.png (zusammengesetzt
vor dem Speichern), after.png (nach .aps speichern + laden) und flat.png (nach
Flatten) muessen pixelgleich sein.

  tools/layertest.py <dir mit before/after/flat.png>
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fmttest as f

d = sys.argv[1]
_, _, b = f.png_read(f"{d}/before.png")
bad = 0
for n in ("after", "flat"):
    _, _, a = f.png_read(f"{d}/{n}.png")
    diff = sum(1 for p, q in zip(b, a) if p != q)
    bad += diff > 0
    print(f"{'ok  ' if not diff else 'FAIL'} {n}: {diff} pixels differ")
print("ALL OK" if not bad else f"{bad} FAILED")
