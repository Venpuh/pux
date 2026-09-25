#!/bin/sh
set -eu
PUX=${1:?path to pux binary required}
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
TMP=$(mktemp -d /tmp/pux-remote-install-test.XXXXXX)
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then kill "$SERVER_PID" 2>/dev/null || true; wait "$SERVER_PID" 2>/dev/null || true; fi
    rm -rf "$TMP"
}
trap cleanup EXIT INT TERM HUP

command -v curl >/dev/null 2>&1 || { echo "curl is required for remote install tests" >&2; exit 1; }
command -v python3 >/dev/null 2>&1 || { echo "python3 is required for remote install tests" >&2; exit 1; }

REMOTE="$TMP/remote"
CACHE="$TMP/cache"
ROOTFS="$TMP/root"
DB="$TMP/db"
TRUSTED="$TMP/trusted"
mkdir -p "$REMOTE" "$CACHE" "$ROOTFS" "$DB" "$TRUSTED"
cp "$ROOT/samples/package-repository"/*.pux "$REMOTE/"
"$PUX" repo create "$REMOTE" >/dev/null
"$PUX" keygen "$TMP/repo.key" "$TMP/repo.pub" >/dev/null
"$PUX" repo sign "$REMOTE" "$TMP/repo.key" >/dev/null
PUX_TRUSTED_KEYS_ROOT="$TRUSTED" "$PUX" trust add "$TMP/repo.pub" >/dev/null

python3 - "$REMOTE" >"$TMP/server.out" 2>/dev/null <<'PYREMOTE' &
import http.server, os, sys
os.chdir(os.path.abspath(sys.argv[1]))
class Quiet(http.server.SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Quiet)
print(server.server_port, flush=True)
server.serve_forever()
PYREMOTE
SERVER_PID=$!
PORT=""
for _ in $(seq 1 50); do
    if [ -s "$TMP/server.out" ]; then PORT=$(head -n 1 "$TMP/server.out"); break; fi
    sleep 0.05
done
[ -n "$PORT" ]

PUX_TRUSTED_KEYS_ROOT="$TRUSTED" PUX_REQUIRE_SIGNED_REPOSITORY=1 PUX_ROOT="$ROOTFS" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 \
    "$PUX" install hello "http://127.0.0.1:$PORT/" "$CACHE" >"$TMP/out" 2>"$TMP/err"

grep -q '^installed: glibc 2.44-1 x86_64$' "$TMP/out"
grep -q '^installed: libidn2 2.3.7-1 x86_64$' "$TMP/out"
grep -q '^installed: hello 1.0.0-1 x86_64$' "$TMP/out"
[ -f "$ROOTFS/usr/bin/hello" ]
[ -f "$CACHE/index.pux" ]
[ -f "$CACHE/index.pux.sig" ]
[ -f "$CACHE/hello-1.0.0-1-x86_64.pux" ]
[ -f "$CACHE/glibc-2.44-1-x86_64.pux" ]
[ -f "$CACHE/libidn2-2.3.7-1-x86_64.pux" ]

# Reuse cached packages for an already-installed root.
PUX_TRUSTED_KEYS_ROOT="$TRUSTED" PUX_REQUIRE_SIGNED_REPOSITORY=1 PUX_ROOT="$ROOTFS" PUX_DB_ROOT="$DB" PUX_ARCH=x86_64 \
    "$PUX" install hello "http://127.0.0.1:$PORT/" "$CACHE" >"$TMP/out2"
grep -q '^already installed: hello 1.0.0-1 x86_64$' "$TMP/out2"

# Same-size remote tampering must be rejected by the indexed SHA-256.
printf X | dd of="$REMOTE/hello-1.0.0-1-x86_64.pux" bs=1 seek=5120 conv=notrunc status=none
rm -f "$CACHE/hello-1.0.0-1-x86_64.pux"
rm -rf "$TMP/tamper-db" "$TMP/tamper-root"
if PUX_TRUSTED_KEYS_ROOT="$TRUSTED" PUX_REQUIRE_SIGNED_REPOSITORY=1 PUX_ROOT="$TMP/tamper-root" PUX_DB_ROOT="$TMP/tamper-db" PUX_ARCH=x86_64 \
    "$PUX" install hello "http://127.0.0.1:$PORT/" "$CACHE" >"$TMP/out3" 2>"$TMP/err3"; then
    echo "tampered remote package unexpectedly installed" >&2
    exit 1
fi
grep -q 'SHA-256 mismatch' "$TMP/err3"
[ ! -e "$TMP/tamper-root/usr/bin/hello" ]

# Missing dependency package must fail before any install-root changes.
rm -f "$REMOTE/libidn2-2.3.7-1-x86_64.pux" "$CACHE/libidn2-2.3.7-1-x86_64.pux"
rm -rf "$TMP/missing-root" "$TMP/missing-db" "$TMP/missing-cache"
mkdir -p "$TMP/missing-cache"
if PUX_TRUSTED_KEYS_ROOT="$TRUSTED" PUX_REQUIRE_SIGNED_REPOSITORY=1 PUX_ROOT="$TMP/missing-root" PUX_DB_ROOT="$TMP/missing-db" PUX_ARCH=x86_64 \
    "$PUX" install hello "http://127.0.0.1:$PORT/" "$TMP/missing-cache" >"$TMP/out4" 2>"$TMP/err4"; then
    echo "missing remote dependency unexpectedly installed" >&2
    exit 1
fi
grep -q 'download returned HTTP 404' "$TMP/err4"
[ ! -e "$TMP/missing-root/usr/bin/hello" ]

echo 'pux remote install tests: OK'
