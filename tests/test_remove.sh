#!/bin/sh
set -eu
PUX=${1:-./build/pux}
TMP=$(mktemp -d /tmp/pux-remove-test.XXXXXX)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ROOT="$TMP/root"
DB="$TMP/db"
mkdir -p "$ROOT" "$DB"

make_pkg() {
    name=$1
    version=$2
    dep=$3
    mkdir -p "$TMP/payload-$name/usr/bin" "$TMP/payload-$name/usr/share/doc/$name"
    printf '#!/bin/sh\necho %s\n' "$name" > "$TMP/payload-$name/usr/bin/$name"
    chmod 755 "$TMP/payload-$name/usr/bin/$name"
    printf '%s documentation\n' "$name" > "$TMP/payload-$name/usr/share/doc/$name/README"
    {
        echo 'format=1'
        echo "name=$name"
        echo "version=$version"
        echo 'release=1'
        echo 'arch=x86_64'
        echo "description=$name removal test package"
        echo 'license=MIT'
        if [ -n "$dep" ]; then echo "depends=$dep"; fi
        echo "provides=$name"
    } > "$TMP/$name.manifest"
    PUX_ARCH=x86_64 "$PUX" build "$TMP/$name.manifest" "$TMP/payload-$name" "$TMP/$name.pux" >/dev/null
}

make_pkg base-runtime 1.0.0 ''
make_pkg app 1.0.0 'base-runtime>=1.0'

PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/base-runtime.pux" >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/app.pux" >/dev/null

# A required package cannot be removed while a dependent is installed.
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove base-runtime >/dev/null 2>&1; then
    echo 'dependent package removal guard failed' >&2
    exit 1
fi
[ -f "$ROOT/usr/bin/base-runtime" ]

# Remove the dependent first; its files and DB record disappear.
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove app >/dev/null
[ ! -e "$ROOT/usr/bin/app" ]
[ ! -e "$ROOT/usr/share/doc/app/README" ]
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" info app >/dev/null 2>&1; then
    echo 'app DB record still exists' >&2
    exit 1
fi

# Now the dependency can be removed as well.
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove base-runtime >/dev/null
[ ! -e "$ROOT/usr/bin/base-runtime" ]
[ ! -e "$ROOT/usr/share/doc/base-runtime/README" ]

# Unowned user data inside a package directory must survive removal.
make_pkg keepdir 1.0.0 ''
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/keepdir.pux" >/dev/null
printf 'user data\n' > "$ROOT/usr/share/doc/keepdir/user.txt"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove keepdir >/dev/null
[ -f "$ROOT/usr/share/doc/keepdir/user.txt" ]
[ ! -e "$ROOT/usr/bin/keepdir" ]

# A symlink replacing an owned regular file must never be followed or removed.
make_pkg victim 1.0.0 ''
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/victim.pux" >/dev/null
rm "$ROOT/usr/bin/victim"
ln -s /etc/passwd "$ROOT/usr/bin/victim"
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove victim >/dev/null 2>&1; then
    echo 'symlink replacement was incorrectly accepted' >&2
    exit 1
fi
[ -L "$ROOT/usr/bin/victim" ]

# Owned symlink is checked against its recorded target during removal.
mkdir -p "$TMP/symlink-payload/usr/bin"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$TMP/symlink-payload/usr/bin/real"
ln -s real "$TMP/symlink-payload/usr/bin/rnano"
cat > "$TMP/symlink.manifest" <<'EOF'
format=1
name=symlinkpkg
version=1.0.0
release=1
arch=x86_64
description=symlink removal test package
license=MIT
provides=symlinkpkg
EOF
PUX_ARCH=x86_64 "$PUX" build "$TMP/symlink.manifest" "$TMP/symlink-payload" "$TMP/symlinkpkg.pux" >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/symlinkpkg.pux" >/dev/null
[ -L "$ROOT/usr/bin/rnano" ]
rm "$ROOT/usr/bin/rnano"
ln -s changed "$ROOT/usr/bin/rnano"
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove symlinkpkg >/dev/null 2>&1; then
    echo 'changed symlink target was incorrectly accepted' >&2
    exit 1
fi
rm "$ROOT/usr/bin/rnano"
ln -s real "$ROOT/usr/bin/rnano"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" remove symlinkpkg >/dev/null
[ ! -e "$ROOT/usr/bin/rnano" ]

printf 'pux remove tests: OK\n'
