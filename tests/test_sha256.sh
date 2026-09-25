#!/bin/sh
set -eu
PUX=${1:?pux binary required}
TMP=$(mktemp -d /tmp/pux-sha256-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

printf '' > "$TMP/empty"
printf 'abc' > "$TMP/abc"

[ "$("$PUX" package checksum "$TMP/empty")" = \
  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ]
[ "$("$PUX" package checksum "$TMP/abc")" = \
  "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" ]

EXPECTED=$(sha256sum "$TMP/abc" | awk '{print $1}')
ACTUAL=$("$PUX" package checksum "$TMP/abc")
[ "$ACTUAL" = "$EXPECTED" ]

echo 'pux SHA-256 tests: OK'
