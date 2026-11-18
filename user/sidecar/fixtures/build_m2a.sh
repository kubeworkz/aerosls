#!/bin/sh
# Build the M2a pair: a dynamic, non-PIE musl hello and the loader it names.
#
# The main image is linked the way the committed fixture was: musl's own
# crt1.o, -nostartfiles, and a temporary libc.so that is a symlink to the
# COMMITTED loader. The loader-as-libc choice is load-bearing twice:
#   - the loader has no SONAME, so the link records DT_NEEDED "libc.so" —
#     the name this demo's image describes — instead of a host libc soname
#     the M2a path does not ship;
#   - the only libc the link can resolve against is the file beside this
#     script, so a regenerate cannot silently pick a different libc.
#
# Requires a musl compiler wrapper (musl-gcc by default) and musl's crt1.o.
# The committed binaries are the reference pair; rerun this only when
# intentionally regenerating them, and keep both files together.
#
# Overrides:
#   MUSL_CC      compiler wrapper (default: musl-gcc)
#   MUSL_CRT1    musl's crt1.o (default: probed in the usual install dirs)
#   MUSL_LOADER  host loader to copy in (default: the committed one)
#   M2A_OUT      output image (default: m2a_hello.elf)
set -eu
cd "$(dirname "$0")"

CC=${MUSL_CC:-musl-gcc}
OUT=${M2A_OUT:-m2a_hello.elf}

# A supplied loader replaces the committed one; the default is already here,
# and copying a file onto itself fails.
if [ -n "${MUSL_LOADER:-}" ]; then
    cp "$MUSL_LOADER" ld-musl-x86_64.so.1
fi
[ -f ld-musl-x86_64.so.1 ] || {
    echo "m2a loader not found: ld-musl-x86_64.so.1" >&2
    exit 1
}

# musl's crt1.o — never the host's: the glibc crt has a different startup
# contract and does not link against musl's loader. MUSL_CRT1 wins; otherwise
# probe the wrapper's sibling libdir and the common install locations.
CRT1=${MUSL_CRT1:-}
if [ -z "$CRT1" ]; then
    CC_PATH=$(command -v "$CC" 2>/dev/null || true)
    CC_DIR=""
    [ -n "$CC_PATH" ] && CC_DIR=$(dirname "$CC_PATH")
    for d in \
        "${CC_DIR:+$CC_DIR/../lib/x86_64-linux-musl}" \
        /usr/lib/x86_64-linux-musl \
        /usr/lib/musl/lib \
        /usr/local/musl/lib; do
        [ -n "$d" ] || continue
        if [ -f "$d/crt1.o" ]; then
            CRT1="$d/crt1.o"
            break
        fi
    done
fi
[ -n "$CRT1" ] && [ -f "$CRT1" ] || {
    echo "musl crt1.o not found; set MUSL_CRT1 to musl's crt1.o" >&2
    exit 1
}

# The temporary -L directory IS the loader-as-libc recipe: one libc.so, the
# committed loader. -nostartfiles keeps the wrapper's startfile choice (PIE
# Scrt1.o under musl-gcc) out of an ET_EXEC link; crt1.o is named explicitly.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ln -s "$(pwd)/ld-musl-x86_64.so.1" "$tmp/libc.so"

"$CC" -nostartfiles -no-pie -O2 -L"$tmp" "$CRT1" -o "$OUT" m2a_hello.c -lc

# Fail closed if the compiler produced the wrong shape: the kernel's M2a
# path needs an ET_EXEC main image with PT_INTERP naming this loader, an
# ET_DYN loader whose PT_LOADs start at vaddr 0, and DT_NEEDED "libc.so" from
# the loader-as-libc link (a host libc would record a soname the demo does
# not ship).
ENTRY_TYPE=$(readelf -h "$OUT" | awk '/Type:/ {print $2}')
INTERP_PATH=$(readelf -l "$OUT" |
    sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
NEEDED=$(readelf -d "$OUT" |
    sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
LOADER_TYPE=$(readelf -h ld-musl-x86_64.so.1 | awk '/Type:/ {print $2}')
[ "$ENTRY_TYPE" = "EXEC" ] || { echo "m2a hello is not ET_EXEC" >&2; exit 1; }
[ "$INTERP_PATH" = "/lib/ld-musl-x86_64.so.1" ] || {
    echo "m2a hello PT_INTERP is '$INTERP_PATH'" >&2; exit 1; }
[ "$NEEDED" = "libc.so" ] || {
    echo "m2a hello DT_NEEDED is '$NEEDED' (want libc.so)" >&2; exit 1; }
[ "$LOADER_TYPE" = "DYN" ] || { echo "m2a loader is not ET_DYN" >&2; exit 1; }

echo "built $OUT: ET_EXEC, PT_INTERP /lib/ld-musl-x86_64.so.1, DT_NEEDED libc.so"
