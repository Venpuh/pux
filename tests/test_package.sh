#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VALID="$ROOT/samples/hello.pux.manifest"
TMPDIR=${TMPDIR:-/tmp}/pux-package-test.$$
trap 'rm -rf "$TMPDIR"' EXIT HUP INT TERM
mkdir -p "$TMPDIR"

output="$($PUX package validate "$VALID")"
[ "$output" = "manifest: valid" ]

info_output="$($PUX package info "$VALID")"
printf '%s\n' "$info_output" | grep -q '^name=hello$'
printf '%s\n' "$info_output" | grep -q '^version=1.0.0$'
printf '%s\n' "$info_output" | grep -q '^depends=glibc>=2.44$'
printf '%s\n' "$info_output" | grep -q '^provides=hello$'

cat > "$TMPDIR/bad.manifest" <<'MANIFEST'
format=99
name=broken
version=1.0.0
release=1
arch=x86_64
description=broken package
license=MIT
MANIFEST

if "$PUX" package validate "$TMPDIR/bad.manifest" >"$TMPDIR/out" 2>"$TMPDIR/err"; then
    echo "expected invalid format to fail" >&2
    exit 1
fi

grep -q 'unsupported manifest format' "$TMPDIR/err"

echo "pux package tests: OK"
