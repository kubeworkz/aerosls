#!/usr/bin/env bash
#
# no_hosted_link_check.sh — asserts that the linked kernel image is
# freestanding end to end: no undefined symbols, no dynamic section, and
# no ELF interpreter. No glibc, no user space.
#
# ─── Why this is a build gate and not a code review note ───────────────────
# AeroSLS is a single-level storage architecture. There is no user space
# and no glibc: the image boots on its own, allocates from its own fixed
# pool, and every symbol in it resolves at link time. That property is
# core identity, and like the TLS fix before it, it is exactly the kind of
# thing a build change can silently undo:
#
#   - a new vendored translation unit calling a real libc symbol
#     (declares fine, resolves to nothing at link time);
#   - -lc (or a crt/startup object) sneaking into X86_LDFLAGS, or a
#     dynamic-linked object linked into the kernel;
#   - a -fno-builtin removal letting the compiler lower memcpy-shaped
#     loops to libc builtins;
#   - a hosted-built object dragged in with dynamic relocations.
#
# The vendored mbedTLS shims (vendor/mbedtls/shim/) prove the point about
# WHERE to check. They satisfy the COMPILER's need for declarations and
# nothing more; whether a hosted call actually leaks into the build is
# decided at the LINK, after every include path, macro, and flag had its
# say — the same argument no_tls_relocations_check.sh makes for
# relocations. Checking the headers would check intent; checking the image
# checks the artifact the machine actually boots.
#
# ─── The three checks ──────────────────────────────────────────────────────
#   1. No undefined symbols (readelf -s, Ndx=UND). An undefined symbol is
#      the only possible glibc reference point. This kernel has no dynamic
#      loader to resolve one, so a weak undefined stays 0 and a call
#      through it dereferences NULL.
#   2. No dynamic-loading artefacts (.dynamic/.dynsym/.dynstr/.interp
#      sections, or DYNAMIC/INTERP program headers). They exist only when
#      something expects to be relocated at load time by a loader the
#      kernel does not have.
#   3. No ELF interpreter (PT_INTERP). An interpreter means the image is
#      meant to be handed to /lib64/ld-linux — a user space this
#      architecture does not contain.
#
# Usage:
#   tests/no_hosted_link_check.sh                 # uses my_sls_kernel.bin
#   tests/no_hosted_link_check.sh path/to/kernel
#
# Exit: 0 pass, 1 fail, 2 abort (prerequisite missing: no binary, no
# binutils).
set -u
cd "$(dirname "$0")/.."   # repo root, so the default kernel path resolves

KERNEL="${1:-my_sls_kernel.bin}"

echo "no_hosted_link_check"
echo "===================="

[ -f "$KERNEL" ] || { echo "ABORT: $KERNEL not found -- run 'make' first" >&2; exit 2; }
command -v readelf >/dev/null || { echo "ABORT: readelf not found (binutils)" >&2; exit 2; }

fails=0

# ─── 1. no undefined symbols ───────────────────────────────────────────────
# NF>7 excludes the null symbol table entry (Ndx=UND, no name), which
# readelf prints on every binary and is not an undefined symbol.
UND="$(readelf -Ws "$KERNEL" 2>/dev/null | awk '$7=="UND" && NF>7 {print $NF}' | sort -u)"
if [ -n "$UND" ]; then
    fails=$((fails + 1))
    echo "FAIL: $KERNEL has undefined symbol(s) -- nothing in the image can resolve them"
    echo "$UND" | sed 's/^/      /'
fi

# ─── 2. no dynamic-loading artefacts ───────────────────────────────────────
DYN="$( { readelf -S "$KERNEL" 2>/dev/null | grep -E '\.dynamic|\.dynsym|\.dynstr|\.interp'
          readelf -l "$KERNEL" 2>/dev/null | grep -E 'DYNAMIC|INTERP'; } | tr -s ' ' | sed 's/^ *//' )"
if [ -n "$DYN" ]; then
    fails=$((fails + 1))
    echo "FAIL: $KERNEL carries dynamic-loading artefacts:"
    echo "$DYN" | sed 's/^/      /'
fi

echo
if [ "$fails" -ne 0 ]; then
    cat >&2 <<'EOF'
FAILED: the kernel image is not freestanding.

  Undefined symbols, a dynamic section, or an interpreter each mean
  something at run time must come from OUTSIDE the image -- a loader, a
  library, a resolved symbol. This kernel has none of those: a symbol no
  image defines is a call through NULL, and a PT_INTERP is a request to
  run a user space that does not exist.

  Check X86_LDFLAGS for anything linking libc or a crt file, and grep the
  newest objects in X86_C_SRC/MBEDTLS_SRC for calls to symbols the kernel
  does not define. The guard on the freestanding side is the binary, not
  the headers.
EOF
    exit 1
fi
echo "PASS: $KERNEL is freestanding -- 0 undefined symbols, no dynamic section, no interpreter."
exit 0
