#!/bin/sh
set -eu
PUX=${1:?pux binary required}
TMP=$(mktemp -d /tmp/pux-trust-test.XXXXXX)
trap 'rm -rf "$TMP"' EXIT HUP INT TERM

repo="$TMP/repo"
keys="$TMP/trusted-keys"
private="$TMP/repo.key"
public="$TMP/repo.pub"
mkdir -p "$repo" "$keys"
cp samples/package-repository/glibc-2.44-1-x86_64.pux "$repo/"
cp samples/package-repository/libidn2-2.3.7-1-x86_64.pux "$repo/"
cp samples/package-repository/hello-1.0.0-1-x86_64.pux "$repo/"

"$PUX" repo create "$repo" >/dev/null
"$PUX" keygen "$private" "$public" >/dev/null
# The trusted-key add output provides the signer keyid.
"$PUX" repo sign "$repo" "$private" >/dev/null
trusted_output=$(PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" trust add "$public")
echo "$trusted_output" | grep '^trusted: [0-9a-f]\{64\}$' >/dev/null
signer=$(printf '%s\n' "$trusted_output" | awk '{print $2}')

PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" trust list | grep "^$signer$" >/dev/null
PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" repo verify-trusted "$repo" | grep "^signature: trusted $signer$" >/dev/null

if PUX_TRUSTED_KEYS_ROOT="$TMP/empty-trust" "$PUX" repo verify-trusted "$repo" >/dev/null 2>&1; then
    echo "untrusted repository unexpectedly verified" >&2
    exit 1
fi

if PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" trust add "$public" >/dev/null 2>&1; then
    echo "duplicate trusted key unexpectedly accepted" >&2
    exit 1
fi

install_root="$TMP/root"
db_root="$TMP/db"
mkdir -p "$install_root" "$db_root"
PUX_ROOT="$install_root" PUX_DB_ROOT="$db_root" PUX_ARCH=x86_64 \
PUX_REQUIRE_SIGNED_REPOSITORY=1 PUX_TRUSTED_KEYS_ROOT="$keys" \
"$PUX" install hello "$repo" >/dev/null
PUX_DB_ROOT="$db_root" PUX_ROOT="$install_root" "$PUX" list | grep '^hello ' >/dev/null

cp "$repo/index.pux" "$TMP/index.good"
printf '\n' >> "$repo/index.pux"
if PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" repo verify-trusted "$repo" >/dev/null 2>&1; then
    echo "tampered repository unexpectedly verified" >&2
    exit 1
fi
if PUX_ROOT="$TMP/strict-root2" PUX_DB_ROOT="$TMP/strict-db2" PUX_ARCH=x86_64 \
PUX_REQUIRE_SIGNED_REPOSITORY=1 PUX_TRUSTED_KEYS_ROOT="$keys" \
"$PUX" install hello "$repo" >/dev/null 2>&1; then
    echo "strict install unexpectedly accepted tampered repository" >&2
    exit 1
fi
cp "$TMP/index.good" "$repo/index.pux"
"$PUX" repo sign "$repo" "$private" >/dev/null

PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" trust remove "$signer" >/dev/null
if PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" repo verify-trusted "$repo" >/dev/null 2>&1; then
    echo "revoked repository unexpectedly verified" >&2
    exit 1
fi

if PUX_TRUSTED_KEYS_ROOT="$keys" "$PUX" trust remove "$signer" >/dev/null 2>&1; then
    echo "missing trusted key unexpectedly removed" >&2
    exit 1
fi

printf 'pux trust tests: OK\n'
