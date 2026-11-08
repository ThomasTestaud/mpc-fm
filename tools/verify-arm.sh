#!/bin/sh
# Static checks of the aarch64 build. They need no device.
set -eu
B="${1:?usage: verify-arm.sh BUILD_DIR}"
fail() { echo "verify-arm: FAIL: $*" >&2; exit 1; }

readelf -h "$B/fm.so" | grep -q 'AArch64' || fail "fm.so is not AArch64"
readelf -h "$B/fm.so" | grep -q 'DYN' || fail "fm.so is not a shared object"
readelf -h "$B/setarch" | grep -q 'AArch64' || fail "setarch is not AArch64"
readelf -lW "$B/setarch" | grep -q 'INTERP' && fail "setarch is not static"

# The device has glibc >= 2.30 (SampQOL needs 2.30). We must not need more.
max=$(readelf -VW "$B/fm.so" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
[ "$(printf '%s\n2.30\n' "$max" | sort -V | tail -1)" = 2.30 ] || fail "fm.so needs GLIBC_$max"

# Only the ALSA functions of the probe may be exported.
exports=$(readelf --dyn-syms -W "$B/fm.so" | awk '$5=="GLOBAL" && $7!="UND" {print $8}' | sort)
for s in $exports; do
    case "$s" in snd_pcm_*) ;; *) fail "unexpected export: $s" ;; esac
done
needed=$(readelf -dW "$B/fm.so" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')

echo "verify-arm: OK"
echo "  fm.so   $(wc -c < "$B/fm.so") bytes, needs GLIBC <= $max, libs: $needed"
echo "  exports $(echo $exports)"
echo "  setarch $(wc -c < "$B/setarch") bytes, static"
