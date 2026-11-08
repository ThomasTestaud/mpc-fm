#!/bin/sh
# Tests of autorun.sh, the SD entry point, with a fake SampQOL folder.
set -eu
. "$(dirname "$0")/lib.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$(cd "$1" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
export MPCFM_TEST_SPLASH_MS=300 # the background status watcher ends fast here
sh "$ROOT/tools/mkdist.sh" "$B" "$T/sd" >/dev/null
A="$T/sd/autorun"

expect "autorun.check matches" "$(cat "$A/autorun.check")" "$(sh "$ROOT/tools/autorun-check.sh" "$A")"
check "autorun.check has no newline" test "$(wc -c < "$A/autorun.check")" -eq 64
check "pins are in autorun.sh" grep -q "fm.so" "$A/autorun.sh"

fake_qol() { # dir: a fake SampQOL folder that prints what it gets
    mkdir -p "$1"
    : > "$1/qol-run.sh"
    : > "$1/mpcqol.so"
    cat > "$1/autorun.sh" <<'QOL'
echo "qol dir=[$(cd "$(dirname "$0")" && pwd)] so=[${MPCFM_SO:-}] off=[${MPCFM_OFF:-}] path1=[${PATH%%:*}]"
QOL
}
fake_qol "$A/sampqol"

out=$(env -u MPCFM_SO MPCFM_RUN="$T/run" sh "$A/autorun.sh")
expect "FM on: SampQOL started with our env" "$out" "qol dir=[$A/sampqol] so=[$T/run/fm.so] off=[] path1=[$T/run/bin]"
check "fm.so copied to RAM" cmp -s "$T/run/fm.so" "$A/mpcfm/fm.so"
check "setarch copied, executable" test -x "$T/run/bin/setarch"

cp "$A/mpcfm/fm.so" "$T/fm.bak"
echo x >> "$A/mpcfm/fm.so"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run2" sh "$A/autorun.sh")
expect "changed fm.so: without FM, strip says why" "$out" "qol dir=[$A/sampqol] so=[] off=[files changed] path1=[$T/run2/bin]"
cp "$T/fm.bak" "$A/mpcfm/fm.so"

touch "$A/mpcfm/DISABLE"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run3" sh "$A/autorun.sh")
expect "DISABLE: without FM, strip says why" "$out" "qol dir=[$A/sampqol] so=[] off=[disabled] path1=[$T/run3/bin]"
rm "$A/mpcfm/DISABLE"

cp "$A/mpcfm/setarch" "$T/shim.bak"
echo x >> "$A/mpcfm/setarch"
P1="${PATH%%:*}"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run5" sh "$A/autorun.sh")
expect "changed setarch: SampQOL alone, PATH untouched" "$out" "qol dir=[$A/sampqol] so=[] off=[] path1=[$P1]"
cp "$T/shim.bak" "$A/mpcfm/setarch"

mv "$A/sampqol" "$A/autorun"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run6" sh "$A/autorun.sh")
expect "SampQOL folder kept its name \"autorun\"" "$out" "qol dir=[$A/autorun] so=[$T/run6/fm.so] off=[] path1=[$T/run6/bin]"
mkdir -p "$A/SampQOL-OS-v0.1.5"
mv "$A/autorun" "$A/SampQOL-OS-v0.1.5/autorun"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run7" sh "$A/autorun.sh")
expect "SampQOL inside its unzipped release folder" "$out" "qol dir=[$A/SampQOL-OS-v0.1.5/autorun] so=[$T/run7/fm.so] off=[] path1=[$T/run7/bin]"
rm -rf "$A/SampQOL-OS-v0.1.5"
fake_qol "$T/sd/sampqol-autorun"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run8" sh "$A/autorun.sh")
expect "SampQOL next to ours at the card root" "$out" "qol dir=[$T/sd/sampqol-autorun] so=[$T/run8/fm.so] off=[] path1=[$T/run8/bin]"
fake_qol "$A/sampqol"
out=$(env -u MPCFM_SO MPCFM_RUN="$T/run9" sh "$A/autorun.sh")
expect "inside ours wins over the card root" "$out" "qol dir=[$A/sampqol] so=[$T/run9/fm.so] off=[] path1=[$T/run9/bin]"
rm -rf "$T/sd/sampqol-autorun" "$A/sampqol"
mkdir -p "$A/sampqol"
: > "$A/sampqol/autorun.sh"

out=$(env -u MPCFM_SO MPCFM_RUN="$T/run4" sh "$A/autorun.sh")
expect "incomplete SampQOL folder: nothing starts" "$out" ""
check "launch log explains" grep -q "no SampQOL OS folder found in" "$A/logs/mpcfm-launch.log"
sleep 1 # let the background watchers finish before the folder goes
finish
