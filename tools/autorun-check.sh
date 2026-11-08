#!/bin/sh
# Print the autorun.check value of an autorun folder.
#
# The stock MPC Sample script runner only starts autorun.sh when
#   autorun.check == sha256(autorun.json || autorun.sh || SALT)
# SALT is a constant of the stock firmware 1.3.0, documented in
# https://github.com/orrpunk-art/mpc-sample-root-ssh (setup_unix.sh).
set -eu
D="${1:?usage: autorun-check.sh AUTORUN_DIR}"
SALT=f7bed1c9-7e90-4731-9194-c996666b4331
{ cat "$D/autorun.json" "$D/autorun.sh"; printf '%s' "$SALT"; } | sha256sum | cut -c1-64
