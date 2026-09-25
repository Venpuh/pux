#!/bin/sh
set -eu
PUX=${1:?path to pux binary required}
TMP=$(mktemp -d /tmp/pux-upgrade-test.XXXXXX)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ROOT="$TMP/root"
DB="$TMP/db"
REPO="$TMP/repo"
mkdir -p "$ROOT" "$DB" "$REPO"

make_pkg() {
    name=$1
    version=$2
    dep=$3
    marker=$4
    conflicts=$5
    payload="$TMP/payload-$name-$version"
    mkdir -p "$payload/usr/bin" "$payload/usr/share/doc/$name"
    printf '#!/bin/sh\necho %s\n' "$marker" > "$payload/usr/bin/$name"
    chmod 755 "$payload/usr/bin/$name"
    printf '%s documentation\n' "$marker" > "$payload/usr/share/doc/$name/README"
    {
        echo 'format=1'
        echo "name=$name"
        echo "version=$version"
        echo 'release=1'
        echo 'arch=x86_64'
        echo "description=$name upgrade test package"
        echo 'license=MIT'
        if [ -n "$dep" ]; then echo "depends=$dep"; fi
        echo "provides=$name"
        if [ -n "$conflicts" ]; then echo "conflicts=$conflicts"; fi
    } > "$TMP/$name-$version.manifest"
    PUX_ARCH=x86_64 "$PUX" build "$TMP/$name-$version.manifest" "$payload" "$TMP/$name-$version.pux" >/dev/null
}

# Basic upgrade: 1.0.0 -> 2.0.0, with the database record and payload replaced.
make_pkg hello 1.0.0 '' old ''
make_pkg hello 2.0.0 '' new ''
cp "$TMP/hello-2.0.0.pux" "$REPO/"

PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/hello-1.0.0.pux" >/dev/null
printf '%s\n' '#!/bin/sh' 'echo old' > "$ROOT/usr/bin/hello"
chmod 755 "$ROOT/usr/bin/hello"
# Restore the expected owned content so the test isolates version replacement.
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" remove hello >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/hello-1.0.0.pux" >/dev/null

PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" upgrade hello "$REPO" >"$TMP/out"
grep -q '^upgraded: hello 1.0.0-1 -> 2.0.0-1$' "$TMP/out"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" info hello >"$TMP/info2"
grep -q '^version=2.0.0$' "$TMP/info2"
printf '#!/bin/sh\necho new\n' > "$TMP/expected-script"
cmp "$TMP/expected-script" "$ROOT/usr/bin/hello"

# A second upgrade is a no-op.
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" upgrade hello "$REPO" >"$TMP/out2"
grep -q '^already up to date: hello 2.0.0-1 x86_64$' "$TMP/out2"

# A dependent package requiring the exact old version must block the upgrade
# before the installed package is changed.
rm -rf "$ROOT" "$DB" "$REPO"
mkdir -p "$ROOT" "$DB" "$REPO"
make_pkg app 1.0.0 'hello=1.0.0' app ''
cp "$TMP/hello-2.0.0.pux" "$REPO/"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/hello-1.0.0.pux" >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/app-1.0.0.pux" >/dev/null
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" upgrade hello "$REPO" >"$TMP/out3" 2>"$TMP/err3"; then
    echo 'upgrade unexpectedly broke an exact-version dependent' >&2
    exit 1
fi
grep -q 'upgrade would break dependency of app: hello=1.0.0' "$TMP/err3"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" info hello >"$TMP/info3"
grep -q '^version=1.0.0$' "$TMP/info3"

# A dependent package with a compatible range allows the upgrade.
rm -rf "$ROOT" "$DB" "$REPO"
mkdir -p "$ROOT" "$DB" "$REPO"
make_pkg app 1.0.0 'hello>=1.0' app-range ''
cp "$TMP/hello-2.0.0.pux" "$REPO/"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/hello-1.0.0.pux" >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/app-1.0.0.pux" >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" upgrade hello "$REPO" >/dev/null
cmp "$TMP/expected-script" "$ROOT/usr/bin/hello"

echo 'pux upgrade tests: OK'
