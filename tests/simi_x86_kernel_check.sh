#!/usr/bin/env bash
# tests/simi_x86_kernel_check.sh — kernel/simi_x86.c must compile clean
# under the kernel's own X86_CFLAGS and link with ZERO undefined symbols.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/simi_x86.c is a direct, unmodified copy of tools/simi/simi_x86.c
# below its header (the same byte-identity convention kernel/simi_arm.c
# follows — plan doc §10.180). Its header claims "no libc dependency,
# mirrors kernel/loader.c's ld_memcpy-style discipline". The ARM kernel
# copy audit (§10.181) proved this class of claim needs enforcement, not
# assertion: GCC 13 on AArch64 synthesized memcpy/memset CALLS from the
# M2 chain struct copies, and only an nm -u check saw them. This guard
# applies the same discipline to the x86 copy: compile it with the exact
# X86_CFLAGS the kernel build uses (mirrored below; the host gcc IS the
# x86 kernel's toolchain — the kernel-guards CI job builds with X86_CC=gcc)
# and assert (a) zero warnings and (b) ZERO undefined symbols. The x86
# kernel links -nostdlib and provides no global memcpy/memset, so any
# symbol at all — including the synthesized pair — would fail the link;
# the contract is strictly empty here, unlike the ARM copy's
# {memcpy, memset} allowance (§10.181).
#
# Toolchain-free: plain host gcc + nm, so it rides the tests/*_check.sh
# glob in tests/run_checks.sh (main CI + kernel-guards --require-all).
#
# Exit: 0 pass, 1 fail, 2 abort (gcc/nm missing, or file missing).
# Optional args (for the teeth smoke): <src.c> <obj>.
set -u
cd "$(dirname "$0")/.."   # repo root

SRC="${1:-kernel/simi_x86.c}"
OBJ="${2:-}"
if [ -z "$OBJ" ]; then
    TMPD=$(mktemp -d)
    trap 'rm -rf "$TMPD"' EXIT
    OBJ="$TMPD/simi_x86.o"
fi

command -v gcc >/dev/null 2>&1 || { echo "ABORT: gcc not found" >&2; exit 2; }
command -v nm  >/dev/null 2>&1 || { echo "ABORT: nm not found" >&2; exit 2; }
[ -f "$SRC" ] || { echo "ABORT: '$SRC' not found" >&2; exit 2; }

# X86_CFLAGS mirror (Makefile): the flags that shape codegen and symbols.
# AB_DEFS and the build-id define are omitted — they add -D's, nothing
# that changes the symbol contract this guard inspects. -Ikernel
# reproduces how the real compile resolves `#include "simi_x86.h"` (the
# source lives in kernel/, so its own directory is searched first); the
# teeth smoke compiles throwaway copies from a temp dir, so it needs the
# include path spelled out — same header, same compile.
CFLAGS="-ffreestanding -O2 -Wall -Wextra -mcmodel=small -mno-red-zone \
        -mno-sse -mno-sse2 -mno-mmx -fno-pie -fno-pic -fno-tree-vectorize \
        -Wframe-larger-than=16384 -Ikernel"

out=$(gcc $CFLAGS -c "$SRC" -o "$OBJ" 2>&1)
if [ -n "$out" ]; then
    printf '%s\n' "FAIL  kernel/simi_x86.c does not compile clean under X86_CFLAGS:" "$out"
    exit 1
fi

bad=$(nm -u "$OBJ" | awk '$1 == "U" { print $2 }')
if [ -n "$bad" ]; then
    printf '%s\n' "FAIL  kernel/simi_x86.o has undefined symbols (the -nostdlib link would fail):" "$bad"
    exit 1
fi

echo "OK    kernel/simi_x86.c compiles clean under X86_CFLAGS with zero undefined symbols"
echo "simi_x86_kernel_check: done, 0 violations"
