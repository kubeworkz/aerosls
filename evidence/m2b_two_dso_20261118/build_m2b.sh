#!/bin/sh
# Build the M2b two-DSO probe: a non-PIE musl hello that needs libc.so and
# libm2a.so, plus the small DSO itself. Evidence-only; not part of the image
# build. The loader-as-libc recipe is the M2a one (see ../../user/sidecar/
# fixtures/build_m2a.sh): DT_NEEDED libc.so is resolved by the loader itself.
#
# Requires a musl compiler wrapper and musl's crt1.o. Overrides:
#   MUSL_CC      compiler wrapper (default: musl-gcc)
#   MUSL_CRT1    musl's crt1.o (required unless musl-gcc is the real wrapper)
#   MUSL_LOADER  loader used as libc.so (default: the committed M2a loader)
#   M2B_OUT      output directory (default: out)
set -eu
cd "$(dirname "$0")"

CC=${MUSL_CC:-musl-gcc}
CRT1=${MUSL_CRT1:-}
LOADER=${MUSL_LOADER:-../../user/sidecar/fixtures/ld-musl-x86_64.so.1}
OUT=${M2B_OUT:-out}

[ -n "$CRT1" ] && [ -f "$CRT1" ] || {
    echo "set MUSL_CRT1 to musl's crt1.o" >&2
    exit 1
}
mkdir -p "$OUT"
ln -sf "$(cd "$(dirname "$LOADER")" && pwd)/$(basename "$LOADER")" \
    "$OUT/libc.so"

"$CC" -shared -fPIC -O2 -nostdlib -Wl,-soname,libm2a.so \
    -o "$OUT/libm2a.so" libm2a.c
"$CC" -nostartfiles -no-pie -O2 -L"$OUT" "$CRT1" \
    -o "$OUT/m2b_hello.elf" m2b_hello.c -lm2a -lc

echo "--- main NEEDED ---"
readelf -d "$OUT/m2b_hello.elf" | grep NEEDED
echo "--- main relocations ---"
readelf -r "$OUT/m2b_hello.elf" | grep -E 'm2a_data|m2a_add'
echo "--- libm2a data relocation ---"
readelf -r "$OUT/libm2a.so" | grep m2a_data
