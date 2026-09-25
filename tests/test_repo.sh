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

SEARCH=$({ "$PUX" search hello "$TMP/repo"; })
printf '%s\n' "$SEARCH" | grep -F 'hello 1.0.0-1 x86_64 GNU Hello example package' >/dev/null

# Index is deterministic when recreated.
cp "$TMP/repo/index.pux" "$TMP/index.before"
"$PUX" repo create "$TMP/repo" >/dev/null
cmp "$TMP/index.before" "$TMP/repo/index.pux"

# Size mismatch is detected.
printf 'x' >> "$TMP/repo/hello-1.0.0-1-x86_64.pux"
if "$PUX" repo validate "$TMP/repo" >/dev/null 2>&1; then
    echo 'size mismatch unexpectedly accepted' >&2
    exit 1
fi

echo 'pux repository tests: OK'
