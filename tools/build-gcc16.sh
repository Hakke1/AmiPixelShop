#!/bin/sh
# Cross-Toolchain mit gcc 16 bauen: bebbo's amiga-gcc, Zweig amiga16.2, dazu
# NDK 3.2, Roadshow-Header (netinclude), vasm, lha, P96- und AmiSSL-SDK.
#
#   sh tools/build-gcc16.sh [PREFIX]      (Vorgabe /opt/amiga16, dauert 30-60 Minuten)
#
# Debian/Ubuntu braucht: make git wget gcc g++ libgmp-dev libmpfr-dev libmpc-dev
# flex bison gettext texinfo libncurses-dev autoconf rsync lhasa
# Danach: make PATH="$PREFIX/bin:$PATH"
set -e
PREFIX=${1:-/opt/amiga16}
WORK=${WORK:-$PWD/build/amiga-gcc}
REPO=https://franke.ms/git/bebbo
AMISSL_URL=https://aminet.net/util/libs/AmiSSL-v5-SDK.lha

[ -d "$WORK/.git" ] || git clone --depth 1 "$REPO/amiga-gcc.git" "$WORK"
cd "$WORK"
cp default-repos .repos
sed -i -E "s#^gcc[[:space:]].*#gcc $REPO/gcc amiga16.2#" .repos
mkdir -p "$PREFIX"
# WERROR=: libnix baut sonst mit -Werror, und gcc 16 warnt dort (maybe-uninitialized)
make -j"$(nproc)" min ndk netinclude vasm lha sfdc fd2sfd PREFIX="$PREFIX" \
     NDK_URL=https://aminet.net/dev/misc/NDK3.2.lha WERROR=
make sdk=p96 PREFIX="$PREFIX"

# AmiSSL: nur die Header (net.c oeffnet die Bibliotheken selbst)
mkdir -p amissl
wget -q -O amissl/sdk.lha "$AMISSL_URL"
(cd amissl && lha -xqf sdk.lha)
cp -R amissl/AmiSSL/Developer/include/. "$PREFIX/m68k-amigaos/include/"

"$PREFIX/bin/m68k-amigaos-gcc" --version | head -n 1
