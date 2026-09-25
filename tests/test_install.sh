#!/bin/sh
set -eu
PUX=${1:-./build/pux}
TMP=$(mktemp -d /tmp/pux-install-test.XXXXXX)
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT INT TERM

ROOT="$TMP/root"
DB="$TMP/db"
mkdir -p "$ROOT" "$DB"

mkdir -p "$TMP/payload/usr/bin" "$TMP/payload/usr/share/doc/simple"
printf '#!/bin/sh\necho simple\n' > "$TMP/payload/usr/bin/simple"
chmod 755 "$TMP/payload/usr/bin/simple"
printf 'simple package\n' > "$TMP/payload/usr/share/doc/simple/README"

cat > "$TMP/simple.manifest" <<'MANIFEST'
format=1
name=simple
version=1.0.0
release=1
arch=x86_64
description=Simple transaction test package
license=MIT
provides=simple
MANIFEST

PUX_ARCH=x86_64 "$PUX" build "$TMP/simple.manifest" "$TMP/payload" "$TMP/simple.pux" >/dev/null

PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/simple.pux" >/dev/null
[ -f "$ROOT/usr/bin/simple" ]
[ -f "$ROOT/usr/share/doc/simple/README" ]
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" list | grep -q '^simple 1.0.0-1 x86_64$'

if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" install "$TMP/simple.pux" >/dev/null 2>&1; then
    echo "reinstall unexpectedly succeeded" >&2
    exit 1
fi

# Existing unowned regular file must block the transaction.
mkdir -p "$TMP/conflict-payload/usr/bin"
printf '#!/bin/sh\necho conflict\n' > "$TMP/conflict-payload/usr/bin/conflict"
chmod 755 "$TMP/conflict-payload/usr/bin/conflict"
cat > "$TMP/conflict.manifest" <<'MANIFEST'
format=1
name=conflict
version=1.0.0
release=1
arch=x86_64
description=Conflict test package
license=MIT
MANIFEST
PUX_ARCH=x86_64 "$PUX" build "$TMP/conflict.manifest" "$TMP/conflict-payload" "$TMP/conflict.pux" >/dev/null
printf 'do not overwrite\n' > "$ROOT/usr/bin/conflict"
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/conflict.pux" >/dev/null 2>&1; then
    echo "conflicting install unexpectedly succeeded" >&2
    exit 1
fi
grep -q '^do not overwrite$' "$ROOT/usr/bin/conflict"

# Missing dependency must be rejected before filesystem changes.
mkdir -p "$TMP/deppayload/usr/bin"
printf '#!/bin/sh\necho dep\n' > "$TMP/deppayload/usr/bin/depapp"
chmod 755 "$TMP/deppayload/usr/bin/depapp"
cat > "$TMP/dep.manifest" <<'MANIFEST'
format=1
name=depapp
version=1.0.0
release=1
arch=x86_64
description=Dependency test package
license=MIT
depends=base-runtime>=1.0
MANIFEST
PUX_ARCH=x86_64 "$PUX" build "$TMP/dep.manifest" "$TMP/deppayload" "$TMP/depapp.pux" >/dev/null
if PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/depapp.pux" >/dev/null 2>&1; then
    echo "missing dependency install unexpectedly succeeded" >&2
    exit 1
fi
[ ! -e "$ROOT/usr/bin/depapp" ]

# Once the required dependency is registered at a sufficient version,
# installation may proceed.
cat > "$TMP/base-runtime.manifest" <<'MANIFEST'
format=1
name=base-runtime
version=1.2.0
release=1
arch=x86_64
description=Registered dependency for transaction test
license=MIT
provides=base-runtime
MANIFEST
: > "$TMP/base-runtime-files.txt"
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" db register \
    "$TMP/base-runtime.manifest" "$TMP/base-runtime-files.txt" >/dev/null
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 "$PUX" install "$TMP/depapp.pux" >/dev/null
[ -f "$ROOT/usr/bin/depapp" ]
PUX_ROOT="$ROOT" PUX_DB_ROOT="$DB" "$PUX" list | grep -q '^depapp 1.0.0-1 x86_64$'

echo 'pux install tests: OK'
