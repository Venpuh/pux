#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}
ROOT=$(mktemp -d /tmp/pux-db-test.XXXXXX)
trap 'rm -rf "$ROOT"' EXIT

export PUX_DB_ROOT="$ROOT/db"

[ "$("$PUX" list)" = "" ]

"$PUX" db register samples/hello.pux.manifest samples/hello-files.txt

list_output="$($PUX list)"
printf '%s\n' "$list_output" | grep -q '^hello 1.0.0-1 x86_64$'

info_output="$($PUX info hello)"
printf '%s\n' "$info_output" | grep -q '^name=hello$'
printf '%s\n' "$info_output" | grep -q '^f usr/bin/hello$'
printf '%s\n' "$info_output" | grep -q '^f usr/share/doc/hello/README$'
printf '%s\n' "$info_output" | grep -q '^d usr/share/doc/hello$'

"$PUX" db unregister hello
[ "$("$PUX" list)" = "" ]

if "$PUX" info hello >/tmp/pux-db-info.err 2>&1; then
    echo "expected missing package lookup to fail" >&2
    exit 1
fi
grep -q 'package is not installed: hello' /tmp/pux-db-info.err
rm -f /tmp/pux-db-info.err

echo "pux database tests: OK"
