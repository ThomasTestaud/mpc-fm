#!/bin/sh
# Runs the fake audio app with fm.so preloaded and checks the status files.
set -eu
. "$(dirname "$0")/lib.sh"
B="$(cd "$1" && pwd)"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
LD_PRELOAD="$B/fm.so" MPCFM_TMPDIR="$T/tmp" MPCFM_LOGDIR="$T/logs" "$B/fake_app"
S="$T/tmp/status.txt"
check "status file written" test -s "$S"
echo "----- $S -----"; cat "$S"; echo "-----"
check "header" grep -q '^mpc-fm .*(FM pads' "$S"
check "session line" grep -q "since start: .* calls" "$S"
check "playback pcm with its name" grep -q 'pcm[0-9] "hw:CARD=I2S,DEV=0" playback' "$S"
check "capture pcm" grep -q 'pcm[0-9] "hw:CARD=I2S,DEV=0" capture' "$S"
check "one xrun and one recover" grep -q 'playback.* xrun 1 recover 1' "$S"
check "fake_app thread listed" grep -q ' fake_app ' "$S"

# The app works 0.8 ms between readi() and writei(): the gap before writei.
gap=$(sed -n 's/^  writei .*gap avg \([0-9.]*\) .*/\1/p' "$S")
check "writei gap avg about 0.8 ms (got $gap)" awk -v g="$gap" 'BEGIN { exit !(g >= 0.75 && g < 2.0) }'
frames=$(sed -n 's/^  writei .* frames *\([0-9]*\) .*/\1/p' "$S")
expect "writei frames per call" "$frames" "128"

check "SD status copy" test -s "$T/logs/mpcfm-status.txt"
check "events: start" grep -q 'start mpc-fm' "$T/logs/mpcfm-events.log"
check "events: xrun" grep -q 'xrun pcm' "$T/logs/mpcfm-events.log"
finish
