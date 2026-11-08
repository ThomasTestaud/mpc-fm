#!/bin/sh
# Tests of the setarch stand-in (host build with -DMPCFM_TEST).
set -eu
. "$(dirname "$0")/lib.sh"
SHIM="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/shim" "$T/real"
cp "$SHIM" "$T/shim/setarch"
printf '#!/bin/sh\necho "PRELOAD=[$LD_PRELOAD] ARGS=[$*]"\n' > "$T/real/setarch"
chmod +x "$T/real/setarch"
: > "$T/fm.so"
Q="$T/no/mpcqol.so"
FM="$Q $T/fm.so"
ARGS='ARGS=[-R -- /usr/bin/MPC Sample]'

run() { # state_dir uptime ld_preload
    mkdir -p "$1"
    env PATH="$T/shim:$T/real:/usr/bin:/bin" MPCFM_BIN="$T/shim" MPCFM_SO="$T/fm.so" \
        MPCFM_STATE="$1" MPCFM_TEST_UPTIME="$2" LD_PRELOAD="$3" \
        setarch -R -- "/usr/bin/MPC Sample" 2>/dev/null
}

S="$T/boot1"
expect "first start: fm.so added after mpcqol.so" "$(run "$S" 100 "$Q")" "PRELOAD=[$FM] $ARGS"
expect "app stopped after 100 s: FM off" "$(run "$S" 200 "$Q")" "PRELOAD=[$Q] $ARGS"
expect "FM stays off in this boot" "$(run "$S" 9000 "$Q")" "PRELOAD=[$Q] $ARGS"
check "launch.log tells why" grep -q "FM off: the app stopped less than 600 s" "$S/launch.log"

S="$T/boot2"
expect "start 1 with FM" "$(run "$S" 100 "$Q")" "PRELOAD=[$FM] $ARGS"
expect "restart after 700 s: FM on" "$(run "$S" 800 "$Q")" "PRELOAD=[$FM] $ARGS"
expect "restart after 700 s: FM on (3rd)" "$(run "$S" 1500 "$Q")" "PRELOAD=[$FM] $ARGS"
expect "4th start: FM off (max 3)" "$(run "$S" 2200 "$Q")" "PRELOAD=[$Q] $ARGS"

S="$T/boot3"
expect "not started by SampQOL: unchanged" "$(run "$S" 100 "/x/other.so")" "PRELOAD=[/x/other.so] $ARGS"
expect "no LD_PRELOAD: unchanged" "$(run "$S" 100 "")" "PRELOAD=[] $ARGS"
expect "fm.so already there: not twice" "$(run "$S" 100 "$FM")" "PRELOAD=[$FM] $ARGS"
check "no state written for those" test ! -e "$S/fm-starts"

S="$T/boot4"
rm "$T/fm.so"
expect "fm.so missing: unchanged" "$(run "$S" 100 "$Q")" "PRELOAD=[$Q] $ARGS"
finish
