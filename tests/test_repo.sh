#!/bin/sh
set -eu
PUX=${1:?pux binary required}
TMP=${TMPDIR:-/tmp}/pux-repo-test.$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/repo"
cp samples/package-repository/*.pux "$TMP/repo/"

"$PUX" repo create "$TMP/repo" >/dev/null
[ -f "$TMP/repo/index.pux" ]
"$PUX" repo validate "$TMP/repo" >/dev/null
grep -Eq '^sha256=[0-9a-f]{64}$' "$TMP/repo/index.pux"

SEARCH=$({ "$PUX" search hello "$TMP/repo"; })
printf '%s\n' "$SEARCH" | grep -F 'hello 1.0.0-1 x86_64 GNU Hello example package' >/dev/null

# Index is deterministic when recreated.
cp "$TMP/repo/index.pux" "$TMP/index.before"
"$PUX" repo create "$TMP/repo" >/dev/null
cmp "$TMP/index.before" "$TMP/repo/index.pux"

# Size mismatch is detected, and validation must fail normally rather than by signal.
printf 'x' >> "$TMP/repo/hello-1.0.0-1-x86_64.pux"
set +e
"$PUX" repo validate "$TMP/repo" >/dev/null 2>"$TMP/size.err"
STATUS=$?
set -e
if [ "$STATUS" -eq 0 ] || [ "$STATUS" -ge 128 ]; then
    echo "size mismatch returned unexpected status $STATUS" >&2
    exit 1
fi
grep -q 'size mismatch' "$TMP/size.err"

# Rebuild the index and verify that same-size content tampering is caught by SHA-256.
cp samples/package-repository/hello-1.0.0-1-x86_64.pux "$TMP/repo/hello-1.0.0-1-x86_64.pux"
"$PUX" repo create "$TMP/repo" >/dev/null
printf X | dd of="$TMP/repo/hello-1.0.0-1-x86_64.pux" bs=1 seek=5120 conv=notrunc status=none
set +e
"$PUX" repo validate "$TMP/repo" >/dev/null 2>"$TMP/hash.err"
STATUS=$?
set -e
if [ "$STATUS" -eq 0 ] || [ "$STATUS" -ge 128 ]; then
    echo "SHA-256 tamper returned unexpected status $STATUS" >&2
    exit 1
fi
grep -q 'SHA-256 mismatch' "$TMP/hash.err"

echo 'pux repository tests: OK'
