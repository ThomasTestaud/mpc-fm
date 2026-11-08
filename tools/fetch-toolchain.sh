#!/bin/sh
# Download the pinned cross toolchain into .tools/ (no root needed).
#
# zig cc cross-compiles C for aarch64 and links against a chosen glibc
# version, so the result runs on the MPC Sample whatever glibc it has
# (>= the target version). See Makefile: TARGET.
set -eu

ZIG_VERSION=0.16.0
ZIG_TARBALL=zig-x86_64-linux-$ZIG_VERSION.tar.xz
ZIG_URL=https://ziglang.org/download/$ZIG_VERSION/$ZIG_TARBALL
ZIG_SHA256=70e49664a74374b48b51e6f3fdfbf437f6395d42509050588bd49abe52ba3d00

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/.tools"
DEST="$TOOLS/zig-$ZIG_VERSION"

if [ -x "$DEST/zig" ]; then
    echo "zig $ZIG_VERSION already in $DEST"
    exit 0
fi

mkdir -p "$TOOLS/download"
cd "$TOOLS/download"
if [ ! -f "$ZIG_TARBALL" ]; then
    echo "downloading $ZIG_URL"
    curl -fsSL --retry 3 -o "$ZIG_TARBALL.part" "$ZIG_URL"
    mv "$ZIG_TARBALL.part" "$ZIG_TARBALL"
fi
echo "$ZIG_SHA256  $ZIG_TARBALL" | sha256sum -c -

rm -rf "$DEST.tmp"
mkdir -p "$DEST.tmp"
tar -xJf "$ZIG_TARBALL" -C "$DEST.tmp" --strip-components=1
mv "$DEST.tmp" "$DEST"
echo "zig $("$DEST/zig" version) installed in $DEST"
