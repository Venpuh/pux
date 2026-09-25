#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
VALID="$ROOT/samples/hello.pux.manifest"
PACKAGE="$ROOT/samples/hello-1.0.0-1-x86_64.pux"
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

archive_output="$($PUX package validate "$PACKAGE")"
[ "$archive_output" = "manifest: valid" ]
archive_info="$($PUX package info "$PACKAGE")"
printf '%s\n' "$archive_info" | grep -q '^name=hello$'
printf '%s\n' "$archive_info" | grep -q '^version=1.0.0$'
printf '%s\n' "$archive_info" | grep -q '^depends=glibc>=2.44$'

# The archive reader must reject absolute and parent-traversal paths.
python3 - "$TMPDIR/unsafe.pux" <<'PY'
import io, tarfile, sys
path = sys.argv[1]
with tarfile.open(path, 'w', format=tarfile.USTAR_FORMAT) as tf:
    manifest = b"format=1\nname=hello\nversion=1.0.0\nrelease=1\narch=x86_64\ndescription=bad\nlicense=MIT\n"
    info = tarfile.TarInfo('META/manifest')
    info.size = len(manifest)
    tf.addfile(info, io.BytesIO(manifest))
    payload = tarfile.TarInfo('../../etc/passwd')
    payload.size = 1
    tf.addfile(payload, io.BytesIO(b'x'))
PY
if "$PUX" package validate "$TMPDIR/unsafe.pux" >"$TMPDIR/out" 2>"$TMPDIR/err"; then
    echo "expected unsafe archive path to fail" >&2
    exit 1
fi
grep -q 'unsafe or unsupported package path' "$TMPDIR/err"

echo "pux package tests: OK"
