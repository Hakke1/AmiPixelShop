#!/bin/sh
# Archiv build/AmiPixelShop.lha und Update-Paket build/AmiPixelShop.upd bauen (nach make)
set -e
pkg=build/pkg/AmiPixelShop
mkdir -p "$pkg/images" "$pkg/ComfyUI"
cp build/AmiPixelShop dist/AmiPixelShop.info LICENSE AmiPixelShop.readme docs/Guide.html "$pkg/"
cp docs/images/*.jpg "$pkg/images/"
cp dist/ComfyUI/* "$pkg/ComfyUI/"
rm -f build/AmiPixelShop.lha
(cd build/pkg && lha -aq ../AmiPixelShop.lha AmiPixelShop)
python3 tools/mkupd.py build/AmiPixelShop.upd
ls -l build/AmiPixelShop build/AmiPixelShop.lha build/AmiPixelShop.upd
