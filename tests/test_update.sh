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

command -v curl >/dev/null 2>&1 || { echo "curl is required for repository update tests" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for repository update tests" >&2; exit 1; }

REMOTE="$TMP/remote"
LOCAL="$TMP/local"
mkdir -p "$REMOTE" "$LOCAL" "$TMP/trusted"
cp "$ROOT/samples/package-repository/hello-1.0.0-1-x86_64.pux" "$REMOTE/"
cp "$ROOT/samples/package-repository/glibc-2.44-1-x86_64.pux" "$REMOTE/"
cp "$ROOT/samples/package-repository/libidn2-2.3.7-1-x86_64.pux" "$REMOTE/"
"$PUX" repo create "$REMOTE" >/dev/null
"$PUX" keygen "$TMP/repo.key" "$TMP/repo.pub" >/dev/null
"$PUX" repo sign "$REMOTE" "$TMP/repo.key" >/dev/null
PUX_TRUSTED_KEYS_ROOT="$TMP/trusted" "$PUX" trust add "$TMP/repo.pub" >/dev/null

python3 - "$REMOTE" >"$TMP/server.out" 2>"$TMP/server.err" <<'PY' &
import http.server, os, sys
from functools import partial
root = os.path.abspath(sys.argv[1])
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass
os.chdir(root)
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

PUX_TRUSTED_KEYS_ROOT="$TMP/trusted" PUX_REQUIRE_SIGNED_REPOSITORY=1 \
    "$PUX" update "http://127.0.0.1:$PORT/" "$LOCAL" >/dev/null
[ -f "$LOCAL/index.pux" ]
[ -f "$LOCAL/index.pux.sig" ]
PUX_TRUSTED_KEYS_ROOT="$TMP/trusted" PUX_REQUIRE_SIGNED_REPOSITORY=1 \
    "$PUX" repo verify-trusted "$LOCAL" >/dev/null

before=$(sha256sum "$LOCAL/index.pux" | awk '{print $1}')
# Tamper with a valid SHA-256 field without resigning: signature verification must reject it.
python3 - "$REMOTE/index.pux" <<'PY'
from pathlib import Path
p = Path(__import__('sys').argv[1])
s = p.read_text()
old = next(line for line in s.splitlines() if line.startswith('sha256='))
current = old.split('=', 1)[1]
replacement = 'f' if current[0] != 'f' else 'e'
s = s.replace(old, 'sha256=' + replacement + current[1:], 1)
p.write_text(s)
PY
if PUX_TRUSTED_KEYS_ROOT="$TMP/trusted" PUX_REQUIRE_SIGNED_REPOSITORY=1 \
    "$PUX" update "http://127.0.0.1:$PORT/" "$LOCAL" >/dev/null 2>&1; then
    echo "tampered repository update unexpectedly succeeded" >&2
    exit 1
fi
after=$(sha256sum "$LOCAL/index.pux" | awk '{print $1}')
[ "$before" = "$after" ]

# Unsigned repositories are accepted when strict signature policy is disabled.
rm -f "$REMOTE/index.pux.sig"
PUX_REQUIRE_SIGNED_REPOSITORY=0 "$PUX" update "http://127.0.0.1:$PORT/" "$TMP/unsigned" >/dev/null
[ -f "$TMP/unsigned/index.pux" ]
[ ! -e "$TMP/unsigned/index.pux.sig" ]

echo "pux update tests: OK"
