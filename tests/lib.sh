# Shared helpers for the shell tests.
FAILS=0
expect() { # name got want
    if [ "$2" = "$3" ]; then
        echo "ok   $1"
    else
        echo "FAIL $1"
        echo "     got:  $2"
        echo "     want: $3"
        FAILS=$((FAILS + 1))
    fi
}
check() { # name command...
    n="$1"; shift
    if "$@"; then echo "ok   $n"; else echo "FAIL $n"; FAILS=$((FAILS + 1)); fi
}
finish() {
    [ "$FAILS" -eq 0 ] && echo "all passed" || { echo "$FAILS failed"; exit 1; }
}
