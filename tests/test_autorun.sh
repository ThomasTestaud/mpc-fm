#!/bin/sh
# Tests of autorun.sh, the SD entry point, with a fake SampQOL folder.
set -eu
. "$(dirname "$0")/lib.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$(cd "$1" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
sh "$ROOT/tools/mkdist.sh" "$B" "$T/sd" >/dev/null
A="$T/sd/autorun"

expect "autorun.check matches" "$(cat "$A/autorun.check")" "$(sh "$ROOT/tools/autorun-check.sh" "$A")"
check "autorun.check has no newline" test "$(wc -c < "$A/autorun.check")" -eq 64
check "pins are in autorun.sh" grep -q "fm.so" "$A/autorun.sh"

mkdir -p "$A/sampqol"
cat > "$A/sampqol/autorun.sh" <<'QOL'
echo "qol dir=[$(cd "$(dirname "$0")" && pwd)] so=[${MPCFM_SO:-}] path1=[${PATH%%:*}]"
QOL
P1="${PATH%%:*}"

out=$(env -u MPCFM_SO MPCFM_RUN="$T/run" sh "$A/autorun.sh")
expect "FM on: SampQOL started with our env" "$out" "qol dir=[$A/sampqol] so=[$T/run/fm.so] path1=[$T/run/bin]"
check "fm.so copied to RAM" cmp -s "$T/run/fm.so" "$A/mpcfm/fm.so"
check "setarch copied, executable" test -x "$T/run/bin/setarch"

cp "$A/mpcfm/fm.so" "$T/fm.bak"
echo x >> "$A/mpcfm/fm.so"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run2" sh "$A/autorun.sh")
expect "changed fm.so: SampQOL without FM" "$out" "qol dir=[$A/sampqol] so=[] path1=[$P1]"
cp "$T/fm.bak" "$A/mpcfm/fm.so"

touch "$A/mpcfm/DISABLE"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run3" sh "$A/autorun.sh")
expect "DISABLE: SampQOL without FM" "$out" "qol dir=[$A/sampqol] so=[] path1=[$P1]"
rm "$A/mpcfm/DISABLE"

rm -rf "$A/sampqol"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run4" sh "$A/autorun.sh")
expect "no SampQOL: nothing starts" "$out" ""
check "launch log explains" grep -q "no SampQOL OS in" "$A/logs/mpcfm-launch.log"
finish
