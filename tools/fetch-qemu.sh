#!/bin/sh
# Unpack Ubuntu/Debian's qemu-user-static into .tools/qemu (no root), to run
# the aarch64 tests (make check-arm).
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/.tools/qemu-deb"
cd "$ROOT/.tools/qemu-deb"
ls qemu-user-static_*.deb >/dev/null 2>&1 || apt-get download qemu-user-static
dpkg-deb -x qemu-user-static_*.deb "$ROOT/.tools/qemu"
"$ROOT/.tools/qemu/usr/bin/qemu-aarch64-static" --version | head -1
