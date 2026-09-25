#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
MANIFEST="$ROOT/samples/hello.pux.manifest"
PAYLOAD="$ROOT/samples/hello-payload"
TMPDIR=${TMPDIR:-/tmp}/pux-build-test.$$
trap 'rm -rf "$TMPDIR"' EXIT HUP INT TERM
mkdir -p "$TMPDIR"

$PUX build "$MANIFEST" "$PAYLOAD" "$TMPDIR/hello.pux"
[ -s "$TMPDIR/hello.pux" ]

validate_output="$($PUX package validate "$TMPDIR/hello.pux")"
[ "$validate_output" = "manifest: valid" ]

info_output="$($PUX package info "$TMPDIR/hello.pux")"
printf '%s\n' "$info_output" | grep -q '^name=hello$'
printf '%s\n' "$info_output" | grep -q '^arch=x86_64$'

# Building the same inputs twice must produce identical bytes.
$PUX build "$MANIFEST" "$PAYLOAD" "$TMPDIR/hello-again.pux"
sha256sum "$TMPDIR/hello.pux" "$TMPDIR/hello-again.pux" > "$TMPDIR/hashes"
awk 'NR==2 { second=$1 } NR==1 { first=$1 } END { exit !(first == second) }' "$TMPDIR/hashes"

# Symlinks are rejected during package creation.
mkdir -p "$TMPDIR/symlink-payload/usr/bin"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$TMPDIR/symlink-payload/usr/bin/real"
ln -s real "$TMPDIR/symlink-payload/usr/bin/link"
if "$PUX" build "$MANIFEST" "$TMPDIR/symlink-payload" "$TMPDIR/symlink.pux" >"$TMPDIR/out" 2>"$TMPDIR/err"; then
    echo "expected symlink payload to fail" >&2
    exit 1
fi
grep -q 'symbolic links are not supported' "$TMPDIR/err"

# Unsupported payload file types are rejected.
mkfifo "$TMPDIR/special"
mkdir -p "$TMPDIR/special-payload"
mv "$TMPDIR/special" "$TMPDIR/special-payload/"
if "$PUX" build "$MANIFEST" "$TMPDIR/special-payload" "$TMPDIR/special.pux" >"$TMPDIR/out" 2>"$TMPDIR/err"; then
    echo "expected special file payload to fail" >&2
    exit 1
fi
grep -q 'unsupported payload file type' "$TMPDIR/err"

echo "pux build tests: OK"
