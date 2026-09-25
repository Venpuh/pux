#!/bin/sh
set -eu
PUX=${1:?path to pux binary required}
TMP=$(mktemp -d /tmp/pux-repo-install-test.XXXXXX)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ROOT="$TMP/root"
DB="$TMP/db"
REPO="$TMP/repo"
mkdir -p "$ROOT" "$DB" "$REPO"

cp samples/package-repository/*.pux "$REPO/"

"$PUX" repo create "$REPO" >/dev/null

if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 \
    "$PUX" install hello "$REPO" >"$TMP/out" 2>"$TMP/err"; then
    :
else
    cat "$TMP/out" >&2 || true
    cat "$TMP/err" >&2 || true
    exit 1
fi

grep -q '^installed: glibc 2.44-1 x86_64$' "$TMP/out"
grep -q '^installed: libidn2 2.3.7-1 x86_64$' "$TMP/out"
grep -q '^installed: hello 1.0.0-1 x86_64$' "$TMP/out"
[ -f "$ROOT/usr/bin/hello" ]
[ -f "$ROOT/usr/share/pux/examples/glibc/README" ]
[ -f "$ROOT/usr/share/pux/examples/libidn2/README" ]

PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install hello "$REPO" >"$TMP/out2"
grep -q '^already installed: glibc 2.44-1 x86_64$' "$TMP/out2"
grep -q '^already installed: libidn2 2.3.7-1 x86_64$' "$TMP/out2"
grep -q '^already installed: hello 1.0.0-1 x86_64$' "$TMP/out2"

# A same-size modification must be rejected by the repository SHA-256.
printf X | dd of="$REPO/hello-1.0.0-1-x86_64.pux" bs=1 seek=5120 conv=notrunc status=none
if PUX_ROOT="$TMP/tampered-root" PUX_DB_ROOT="$TMP/tampered-db" PUX_ARCH=x86_64 \
    "$PUX" install hello "$REPO" >"$TMP/out-tampered" 2>"$TMP/err-tampered"; then
    echo "tampered repository package unexpectedly installed" >&2
    exit 1
fi
grep -q 'SHA-256 mismatch' "$TMP/err-tampered"
[ ! -e "$TMP/tampered-root/usr/bin/hello" ]

# Restore the package before the remaining tests.
cp samples/package-repository/hello-1.0.0-1-x86_64.pux "$REPO/hello-1.0.0-1-x86_64.pux"

# A non-empty repository missing the requested root package must fail before touching the root.
mkdir -p "$TMP/empty-repo"
cp "$REPO/glibc-2.44-1-x86_64.pux" "$TMP/empty-repo/"
if PUX_ROOT="$TMP/empty-root" PUX_DB_ROOT="$TMP/empty-db" PUX_ARCH=x86_64 \
    "$PUX" install does-not-exist "$TMP/empty-repo" >"$TMP/out3" 2>"$TMP/err3"; then
    echo "missing package unexpectedly succeeded" >&2
    exit 1
fi
grep -Eq 'package not found|package .*not found' "$TMP/err3"
[ ! -e "$TMP/empty-root/usr" ]

echo 'pux repository install tests: OK'
