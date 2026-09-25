#!/bin/sh
set -eu

PUX=${1:?pux path required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d)
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM HUP

command -v python3 >/dev/null 2>&1 || { echo "python3 is required for repository config tests" >&2; exit 1; }
command -v curl >/dev/null 2>&1 || { echo "curl is required for repository config tests" >&2; exit 1; }

CONFIG="$TMP/repos.d"
CACHE="$TMP/cache"
TRUST="$TMP/trusted"
REMOTE="$TMP/remote"
mkdir -p "$REMOTE" "$TRUST"
cp "$ROOT/samples/package-repository/hello-1.0.0-1-x86_64.pux" "$REMOTE/"
cp "$ROOT/samples/package-repository/glibc-2.44-1-x86_64.pux" "$REMOTE/"
cp "$ROOT/samples/package-repository/libidn2-2.3.7-1-x86_64.pux" "$REMOTE/"
"$PUX" repo create "$REMOTE" >/dev/null
"$PUX" keygen "$TMP/repo.key" "$TMP/repo.pub" >/dev/null
"$PUX" repo sign "$REMOTE" "$TMP/repo.key" >/dev/null
PUX_TRUSTED_KEYS_ROOT="$TRUST" "$PUX" trust add "$TMP/repo.pub" >/dev/null

python3 - "$REMOTE" >"$TMP/server.out" 2>/dev/null <<'PY' &
import http.server, os, sys
root = os.path.abspath(sys.argv[1])
os.chdir(root)
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args): pass
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
URL="http://127.0.0.1:$PORT"

export PUX_REPO_CONFIG_ROOT="$CONFIG"
export PUX_REPO_CACHE_ROOT="$CACHE"
export PUX_TRUSTED_KEYS_ROOT="$TRUST"
export PUX_REQUIRE_SIGNED_REPOSITORY=0

"$PUX" repo add stable "$URL" 200 1 1 >/dev/null
"$PUX" repo add disabled "$URL" 50 0 0 >/dev/null

list=$($PUX repo list)
printf '%s\n' "$list" | grep '^stable priority=200 enabled=1 signed=1 ' >/dev/null
printf '%s\n' "$list" | grep '^disabled priority=50 enabled=0 signed=0 ' >/dev/null
first=$(printf '%s\n' "$list" | head -n 1)
printf '%s\n' "$first" | grep '^stable ' >/dev/null

"$PUX" repo update stable >/dev/null
[ -f "$CACHE/stable/index.pux" ]
[ -f "$CACHE/stable/index.pux.sig" ]
PUX_REQUIRE_SIGNED_REPOSITORY=0 "$PUX" repo verify-trusted "$CACHE/stable" >/dev/null

rm -f "$CACHE/stable/index.pux" "$CACHE/stable/index.pux.sig"
"$PUX" update >/dev/null
[ -f "$CACHE/stable/index.pux" ]
[ -f "$CACHE/stable/index.pux.sig" ]
[ ! -e "$CACHE/disabled/index.pux" ]

"$PUX" search hello >/dev/null

"$PUX" repo remove disabled >/dev/null
if "$PUX" repo remove disabled >/dev/null 2>&1; then
    echo "second repository removal unexpectedly succeeded" >&2
    exit 1
fi

if "$PUX" repo add '../bad' "$URL" >/dev/null 2>&1; then
    echo "invalid repository name unexpectedly succeeded" >&2
    exit 1
fi
if "$PUX" repo add bad 'file:///tmp/repo' >/dev/null 2>&1; then
    echo "invalid repository URL unexpectedly succeeded" >&2
    exit 1
fi

echo "pux repository configuration tests: OK"
