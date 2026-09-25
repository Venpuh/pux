#!/bin/sh
set -eu
PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=${TMPDIR:-/tmp}/pux-symlink-test.$$
trap 'rm -rf "$TMP"' EXIT HUP INT TERM
mkdir -p "$TMP/payload/usr/bin" "$TMP/root" "$TMP/db"
printf '%s\n' '#!/bin/sh' 'exit 0' > "$TMP/payload/usr/bin/nano"
chmod 755 "$TMP/payload/usr/bin/nano"
ln -s nano "$TMP/payload/usr/bin/rnano"
cat > "$TMP/manifest" <<'EOF'
format=1
name=nano-test
version=9.2
release=1
arch=x86_64
description=Nano symbolic link test package
license=GPL-3.0-or-later
provides=nano-test
EOF
$PUX build "$TMP/manifest" "$TMP/payload" "$TMP/nano-test.pux" >/dev/null
$PUX package validate "$TMP/nano-test.pux" >/dev/null
$PUX package extract "$TMP/nano-test.pux" "$TMP/extracted" >/dev/null
[ -L "$TMP/extracted/usr/bin/rnano" ]
[ "$(readlink "$TMP/extracted/usr/bin/rnano")" = nano ]
PUX_ROOT="$TMP/root" PUX_DB_ROOT="$TMP/db" PUX_ARCH=x86_64 $PUX install "$TMP/nano-test.pux" >/dev/null
[ -L "$TMP/root/usr/bin/rnano" ]
[ "$(readlink "$TMP/root/usr/bin/rnano")" = nano ]
PUX_ROOT="$TMP/root" PUX_DB_ROOT="$TMP/db" $PUX remove nano-test >/dev/null
[ ! -e "$TMP/root/usr/bin/rnano" ]
echo "pux symlink tests: OK"
