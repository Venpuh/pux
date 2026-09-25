#!/bin/sh
set -eu
PUX=${1:?pux binary required}
TMP=$(mktemp -d /tmp/pux-signature-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

repo="$TMP/repo"
private="$TMP/repo.key"
public="$TMP/repo.pub"
wrong_private="$TMP/wrong.key"
wrong_public="$TMP/wrong.pub"
mkdir -p "$repo"
cp samples/hello-1.0.0-1-x86_64.pux "$repo/"
cp samples/package-repository/glibc-2.44-1-x86_64.pux "$repo/"
cp samples/package-repository/libidn2-2.3.7-1-x86_64.pux "$repo/"
cp samples/package-repository/hello-1.0.0-1-x86_64.pux "$repo/"

"$PUX" repo create "$repo" >/dev/null
keygen_output=$("$PUX" keygen "$private" "$public")
printf '%s\n' "$keygen_output" | grep '^keyid: [0-9a-f][0-9a-f]' >/dev/null
[ "$(stat -c '%a' "$private")" = "600" ]
"$PUX" repo sign "$repo" "$private" >/dev/null
"$PUX" repo verify "$repo" "$public" >/dev/null

cp "$repo/index.pux" "$TMP/index.original"
sig_before=$(sha256sum "$repo/index.pux.sig" | awk '{print $1}')
[ -n "$sig_before" ]

printf '\n' >> "$repo/index.pux"
if "$PUX" repo verify "$repo" "$public" >/dev/null 2>&1; then
    echo "tampered index unexpectedly verified" >&2
    exit 1
fi
cp "$TMP/index.original" "$repo/index.pux"
# Restore a freshly signed copy after the deliberate tamper.
"$PUX" repo sign "$repo" "$private" >/dev/null
"$PUX" repo verify "$repo" "$public" >/dev/null

"$PUX" keygen "$wrong_private" "$wrong_public" >/dev/null
if "$PUX" repo verify "$repo" "$wrong_public" >/dev/null 2>&1; then
    echo "wrong public key unexpectedly verified" >&2
    exit 1
fi

cat > "$TMP/bad.pub" <<'KEY'
# pux-ed25519-public=1
algorithm=ed25519
keyid=0000000000000000000000000000000000000000000000000000000000000000
public=0000000000000000000000000000000000000000000000000000000000000000
KEY
if "$PUX" repo verify "$repo" "$TMP/bad.pub" >/dev/null 2>&1; then
    echo "forged public key unexpectedly verified" >&2
    exit 1
fi

cat > "$TMP/bad-alg.pub" <<'KEY'
# pux-ed25519-public=1
algorithm=not-ed25519
keyid=0000000000000000000000000000000000000000000000000000000000000000
public=0000000000000000000000000000000000000000000000000000000000000000
KEY
if "$PUX" repo verify "$repo" "$TMP/bad-alg.pub" >/dev/null 2>&1; then
    echo "wrong algorithm unexpectedly verified" >&2
    exit 1
fi

printf 'pux signature tests: OK\n'
