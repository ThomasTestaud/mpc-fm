#!/bin/sh
# Build the SD card folder: OUT/autorun/ (our files only, no SampQOL files).
#   usage: mkdist.sh BUILD_DIR OUT_DIR   (BUILD_DIR has fm.so and setarch)
set -eu
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:?usage: mkdist.sh BUILD_DIR OUT_DIR}"
OUT="${2:?usage: mkdist.sh BUILD_DIR OUT_DIR}"
VERSION=$(sed -n 's/^#define MPCFM_VERSION "\(.*\)"$/\1/p' "$ROOT/src/version.h")
[ -n "$VERSION" ] || { echo "mkdist: no version in src/version.h" >&2; exit 1; }

A="$OUT/autorun"
# Replace only our own files: keep logs/ and anything else put there.
rm -rf "$A/mpcfm" "$A/autorun.sh" "$A/autorun.json" "$A/autorun.check"
mkdir -p "$A/mpcfm"
cp "$BIN/fm.so" "$BIN/setarch" "$A/mpcfm/"

PINS=$(cd "$A/mpcfm" && sha256sum fm.so setarch)
while IFS= read -r line; do
    case "$line" in
        'PINS="@PINS@"') printf 'PINS="%s"\n' "$PINS" ;;
        *) printf '%s\n' "$line" ;;
    esac
done < "$ROOT/launcher/autorun.sh.in" | sed "s/@VERSION@/$VERSION/g" > "$A/autorun.sh"
grep -q '@PINS@\|@VERSION@' "$A/autorun.sh" && { echo "mkdist: placeholder left" >&2; exit 1; }

cp "$ROOT/launcher/autorun.json" "$A/"
printf '%s' "$(sh "$ROOT/tools/autorun-check.sh" "$A")" > "$A/autorun.check"
cp "$ROOT/launcher/INSTALL.txt" "$A/mpcfm/INSTALL.txt"
echo "mkdist: $A (mpc-fm $VERSION)"
