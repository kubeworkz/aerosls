#!/bin/sh
# Build the M2 dynamic hello: an ET_EXEC with PT_INTERP naming the committed
# musl loader and no libc calls at all. Plain gcc is enough because the
# program is raw syscalls; linking against the loader-as-libc (the M2a
# recipe) is what records PT_INTERP and DT_NEEDED libc.so, which musl's
# loader satisfies from itself.
set -eu
cd "$(dirname "$0")"
[ -f ld-musl-x86_64.so.1 ] || {
    echo "m2a loader not found: ld-musl-x86_64.so.1" >&2; exit 1; }
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ln -s "$(pwd)/ld-musl-x86_64.so.1" "$tmp/libc.so"

gcc -nostdlib -no-pie -Wl,--build-id=none -L"$tmp" \
    -Wl,--dynamic-linker=/lib/ld-musl-x86_64.so.1 \
    -o m2_dyn_hello.elf m2_dyn_hello.S -Wl,--no-as-needed -lc

TYPE=$(readelf -h m2_dyn_hello.elf | awk '/Type:/ {print $2}')
INTERP=$(readelf -l m2_dyn_hello.elf |
    sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
[ "$TYPE" = "EXEC" ] || { echo "m2 dyn hello is not ET_EXEC" >&2; exit 1; }
[ "$INTERP" = "/lib/ld-musl-x86_64.so.1" ] || {
    echo "m2 dyn hello PT_INTERP is '$INTERP'" >&2; exit 1; }
echo "built m2_dyn_hello.elf: ET_EXEC, PT_INTERP $INTERP"
readelf -d m2_dyn_hello.elf | grep NEEDED || true
sha256sum m2_dyn_hello.elf
