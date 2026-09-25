#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PACKAGE="$ROOT/samples/hello-1.0.0-1-x86_64.pux"
TMPDIR=${TMPDIR:-/tmp}/pux-extract-test.$$
trap 'rm -rf "$TMPDIR"' EXIT HUP INT TERM
mkdir -p "$TMPDIR"

$PUX package extract "$PACKAGE" "$TMPDIR/root"
[ -f "$TMPDIR/root/usr/bin/hello" ]
[ -f "$TMPDIR/root/usr/share/doc/hello/README" ]
grep -q 'hello package payload' "$TMPDIR/root/usr/share/doc/hello/README"
[ "$(stat -c '%a' "$TMPDIR/root/usr/bin/hello")" = "755" ]

# Existing files are never overwritten.
if "$PUX" package extract "$PACKAGE" "$TMPDIR/root" >"$TMPDIR/out" 2>"$TMPDIR/err"; then
    echo "expected extraction into existing files to fail" >&2
    exit 1
fi
grep -q 'File exists' "$TMPDIR/err"

# A destination path that ends in a symlink is rejected by O_NOFOLLOW.
mkdir -p "$TMPDIR/real"
ln -s "$TMPDIR/real" "$TMPDIR/link"
if "$PUX" package extract "$PACKAGE" "$TMPDIR/link" >"$TMPDIR/out" 2>"$TMPDIR/err"; then
    echo "expected symlink destination to fail" >&2
    exit 1
fi

echo "pux extract tests: OK"
