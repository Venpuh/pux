#!/bin/sh
set -eu

PUX=${1:?path to pux binary required}
ROOT=$(mktemp -d /tmp/pux-resolver-test.XXXXXX)
REPO="$ROOT/repo"
mkdir -p "$REPO"
trap 'rm -rf "$ROOT"' EXIT

make_manifest() {
    output=$1
    cat > "$output" <<EOF_MANIFEST
format=1
name=$2
version=$3
release=1
arch=x86_64
description=$4
license=MIT
EOF_MANIFEST
    shift 4
    for dep in "$@"; do
        printf 'depends=%s\n' "$dep" >> "$output"
    done
}

make_manifest "$REPO/glibc.pux.manifest" glibc 2.44 'GNU C Library example' 
make_manifest "$REPO/libidn2.pux.manifest" libidn2 2.3.7 'IDN2 example'
cp samples/hello.pux.manifest "$REPO/hello.pux.manifest"

cat >> "$REPO/hello.pux.manifest" <<'EOF_DEPS'
# duplicate dependencies below are deliberately not used; the source manifest already has them
EOF_DEPS

output="$($PUX resolve hello "$REPO")"
printf '%s\n' "$output" | grep -q '^resolution: OK$'
printf '%s\n' "$output" | grep -q '^install: glibc 2.44-1 x86_64$'
printf '%s\n' "$output" | grep -q '^install: libidn2 2.3.7-1 x86_64$'
printf '%s\n' "$output" | grep -q '^install: hello 1.0.0-1 x86_64$'

make_manifest "$REPO/zlib-old.pux.manifest" zlib 1.0.0 'zlib old'
make_manifest "$REPO/zlib-new.pux.manifest" zlib 2.0.0 'zlib new'
make_manifest "$REPO/app.pux.manifest" app 1.0.0 'resolver app' 'zlib>=2.0.0'
output="$($PUX resolve app "$REPO")"
printf '%s\n' "$output" | grep -q '^install: zlib 2.0.0-1 x86_64$'
printf '%s\n' "$output" | grep -q '^install: app 1.0.0-1 x86_64$'

make_manifest "$REPO/provider.pux.manifest" provider 1.0.0 'virtual provider'
printf 'provides=virtual-api\n' >> "$REPO/provider.pux.manifest"
make_manifest "$REPO/consumer.pux.manifest" consumer 1.0.0 'virtual consumer' virtual-api
output="$($PUX resolve consumer "$REPO")"
printf '%s\n' "$output" | grep -q '^install: provider 1.0.0-1 x86_64$'
printf '%s\n' "$output" | grep -q '^install: consumer 1.0.0-1 x86_64$'

make_manifest "$REPO/missing.pux.manifest" missing 1.0.0 'missing dep' does-not-exist
if "$PUX" resolve missing "$REPO" >/tmp/pux-resolver-missing.err 2>&1; then
    echo "expected missing dependency to fail" >&2
    exit 1
fi
grep -q 'no package satisfies dependency: does-not-exist' /tmp/pux-resolver-missing.err
rm -f /tmp/pux-resolver-missing.err

make_manifest "$REPO/cycle-a.pux.manifest" cycle-a 1.0.0 'cycle a' cycle-b
make_manifest "$REPO/cycle-b.pux.manifest" cycle-b 1.0.0 'cycle b' cycle-a
output="$($PUX resolve cycle-a "$REPO")"
printf '%s\n' "$output" | grep -q '^install: cycle-b 1.0.0-1 x86_64$'
printf '%s\n' "$output" | grep -q '^install: cycle-a 1.0.0-1 x86_64$'

make_manifest "$REPO/conflict-one.pux.manifest" conflict-one 1.0.0 'conflict one' conflict-two
make_manifest "$REPO/conflict-two.pux.manifest" conflict-two 1.0.0 'conflict two'
printf 'conflicts=conflict-one\n' >> "$REPO/conflict-two.pux.manifest"
if "$PUX" resolve conflict-one "$REPO" >/tmp/pux-resolver-conflict.err 2>&1; then
    echo "expected conflict to fail" >&2
    exit 1
fi
grep -q 'candidate conflicts with the current resolution: conflict-two' /tmp/pux-resolver-conflict.err
rm -f /tmp/pux-resolver-conflict.err

printf 'noarch example\n'
cat > "$REPO/noarch-tool.pux.manifest" <<'EOF_NOARCH'
format=1
name=noarch-tool
version=1.0.0
release=1
arch=noarch
description=Noarch example
license=MIT
EOF_NOARCH
make_manifest "$REPO/noarch-app.pux.manifest" noarch-app 1.0.0 'noarch consumer' noarch-tool
output="$($PUX resolve noarch-app "$REPO")"
printf '%s\n' "$output" | grep -q '^install: noarch-tool 1.0.0-1 noarch$'

# Wrong-architecture candidates must not satisfy x86_64 resolution.
cat > "$REPO/wrong-arch.pux.manifest" <<'EOF_WRONG'
format=1
name=wrong-arch
version=1.0.0
release=1
arch=aarch64
description=Wrong architecture
license=MIT
EOF_WRONG
cat > "$REPO/needs-wrong-arch.pux.manifest" <<'EOF_NEEDS'
format=1
name=needs-wrong-arch
version=1.0.0
release=1
arch=x86_64
description=Architecture test
license=MIT
depends=wrong-arch
EOF_NEEDS
if "$PUX" resolve needs-wrong-arch "$REPO" >/tmp/pux-resolver-arch.err 2>&1; then
    echo "expected wrong architecture to fail" >&2
    exit 1
fi
grep -q 'no package satisfies dependency: wrong-arch' /tmp/pux-resolver-arch.err
rm -f /tmp/pux-resolver-arch.err

echo "pux resolver tests: OK"
