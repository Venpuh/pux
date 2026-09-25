#!/bin/sh
set -eu
PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d /tmp/pux-configured-install-test.XXXXXX)
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM HUP

command -v curl >/dev/null 2>&1 || { echo "curl is required for configured install tests" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for configured install tests" >&2; exit 1; }

REMOTE="$TMP/remote"
CACHE="$TMP/cache"
CONFIG="$TMP/repos.d"
ROOTFS="$TMP/root"
DB="$TMP/db"
TRUSTED="$TMP/trusted"
mkdir -p "$REMOTE" "$CACHE" "$ROOTFS" "$DB" "$TRUSTED"
cp "$ROOT/samples/package-repository"/*.pux "$REMOTE/"

"$PUX" repo create "$REMOTE" >/dev/null
"$PUX" keygen "$TMP/repo.key" "$TMP/repo.pub" >/dev/null
"$PUX" repo sign "$REMOTE" "$TMP/repo.key" >/dev/null
PUX_TRUSTED_KEYS_ROOT="$TRUSTED" "$PUX" trust add "$TMP/repo.pub" >/dev/null

python3 - "$REMOTE" >"$TMP/server.out" 2>/dev/null <<'PY' &
import http.server, os, sys
os.chdir(os.path.abspath(sys.argv[1]))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
print(server.server_port, flush=True)
server.serve_forever()
PY
SERVER_PID=$!
PORT=""
for _ in $(seq 1 50); do
    if [ -s "$TMP/server.out" ]; then PORT=$(head -n 1 "$TMP/server.out"); break; fi
    sleep 0.05
done
[ -n "$PORT" ]

export PUX_REPO_CONFIG_ROOT="$CONFIG"
export PUX_REPO_CACHE_ROOT="$CACHE"
export PUX_TRUSTED_KEYS_ROOT="$TRUSTED"
export PUX_REQUIRE_SIGNED_REPOSITORY=0
export PUX_ROOT="$ROOTFS"
export PUX_DB_ROOT="$DB"
export PUX_ARCH=x86_64

"$PUX" repo add stable "http://127.0.0.1:$PORT" 200 1 1 >/dev/null
"$PUX" update stable >/dev/null

"$PUX" install hello >"$TMP/out"
grep -q '^installed: glibc 2.44-1 x86_64$' "$TMP/out"
grep -q '^installed: libidn2 2.3.7-1 x86_64$' "$TMP/out"
grep -q '^installed: hello 1.0.0-1 x86_64$' "$TMP/out"
[ -f "$ROOTFS/usr/bin/hello" ]

"$PUX" install hello >"$TMP/out2"
grep -q '^already installed: hello 1.0.0-1 x86_64$' "$TMP/out2"

# Prepare a newer package in the same configured repository and re-sign its index.
PAYLOAD="$TMP/hello-2.0.0-payload"
mkdir -p "$PAYLOAD/usr/bin" "$PAYLOAD/usr/share/doc/hello"
printf '#!/bin/sh\necho configured-upgrade\n' > "$PAYLOAD/usr/bin/hello"
chmod 755 "$PAYLOAD/usr/bin/hello"
printf 'configured upgrade documentation\n' > "$PAYLOAD/usr/share/doc/hello/README"
cat > "$TMP/hello-2.0.0.manifest" <<'EOF_MANIFEST'
format=1
name=hello
version=2.0.0
release=1
arch=x86_64
description=GNU Hello configured upgrade test package
license=GPL-3.0-or-later
depends=glibc>=2.44
depends=libidn2>=2.3
provides=hello
EOF_MANIFEST
"$PUX" build "$TMP/hello-2.0.0.manifest" "$PAYLOAD" "$REMOTE/hello-2.0.0-1-x86_64.pux" >/dev/null
"$PUX" repo create "$REMOTE" >/dev/null
"$PUX" repo sign "$REMOTE" "$TMP/repo.key" >/dev/null
"$PUX" update stable >/dev/null

"$PUX" upgrade hello >"$TMP/out3"
grep -q '^upgraded: hello 1.0.0-1 -> 2.0.0-1$' "$TMP/out3"
"$PUX" info hello >"$TMP/info"
grep -q '^version=2.0.0$' "$TMP/info"

echo 'pux configured install/upgrade tests: OK'
