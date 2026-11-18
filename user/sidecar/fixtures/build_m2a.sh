#!/bin/sh
# Build the M2a pair: a dynamic, non-PIE musl hello and the loader it names.
#
# Requires a musl cross compiler (`musl-gcc`) and the musl loader on the
# build host. The committed binaries are the reference pair; rerun this only
# when intentionally regenerating them, and keep both files together.
set -eu
cd "$(dirname "$0")"

CC=${MUSL_CC:-musl-gcc}
LOADER=${MUSL_LOADER:-/lib/ld-musl-x86_64.so.1}
LIBDIR=${MUSL_LIBDIR:-/lib/x86_64-linux-musl}

# -L is load-bearing: musl-gcc's specs search the directory holding libc.a
# first, and a main linked against it has no PT_INTERP. Picking the shared
# libc.so makes the ET_EXEC dynamically linked.
"$CC" -no-pie -O2 -L"$LIBDIR" -o m2a_hello.elf m2a_hello.c
cp "$LOADER" ld-musl-x86_64.so.1

# Fail closed if the compiler produced the wrong shape: the kernel's M2a
# path needs an ET_EXEC main image with PT_INTERP naming this loader, and an
# ET_DYN loader whose PT_LOADs start at vaddr 0.
ENTRY_TYPE=$(readelf -h m2a_hello.elf | awk '/Type:/ {print $2}')
INTERP_PATH=$(readelf -l m2a_hello.elf |
    sed -n 's/.*Requesting program interpreter: \(.*\)]/\1/p')
LOADER_TYPE=$(readelf -h ld-musl-x86_64.so.1 | awk '/Type:/ {print $2}')
[ "$ENTRY_TYPE" = "EXEC" ] || { echo "m2a hello is not ET_EXEC" >&2; exit 1; }
[ "$INTERP_PATH" = "/lib/ld-musl-x86_64.so.1" ] || {
    echo "m2a hello PT_INTERP is '$INTERP_PATH'" >&2; exit 1; }
[ "$LOADER_TYPE" = "DYN" ] || { echo "m2a loader is not ET_DYN" >&2; exit 1; }
