#!/bin/sh
# Rebuild fixtures/libm2a.so (M2b's second DSO). The guard pins the bytes'
# SHA-256; MUSL_CC overrides the compiler wrapper.
set -eu
cd "$(dirname "$0")"
CC=${MUSL_CC:-musl-gcc}
"$CC" -shared -fPIC -O2 -nostdlib -Wl,-soname,libm2a.so -o libm2a.so libm2a.c
sha256sum libm2a.so
