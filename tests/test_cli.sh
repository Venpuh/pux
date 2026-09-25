#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}

output="$($PUX version)"
[ "$output" = "pux 0.18.0-dev" ]

help_output="$($PUX help)"
printf '%s\n' "$help_output" | grep -q '^Usage:'
printf '%s\n' "$help_output" | grep -q 'install'
printf '%s\n' "$help_output" | grep -q 'upgrade'
printf '%s\n' "$help_output" | grep -q 'build'
printf '%s\n' "$help_output" | grep -q 'extract'
printf '%s\n' "$help_output" | grep -q 'db          Inspect'
printf '%s\n' "$help_output" | grep -q 'resolve'
printf '%s\n' "$help_output" | grep -q 'checksum'

if "$PUX" definitely-not-a-command >/tmp/pux-test.err 2>&1; then
    echo "expected unknown command to fail" >&2
    exit 1
fi

grep -q "unknown command" /tmp/pux-test.err
rm -f /tmp/pux-test.err

echo "pux CLI tests: OK"
