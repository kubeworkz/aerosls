#!/usr/bin/env bash
# tools/simi/tests/simi_riscv_kernel_check.sh — kernel/simi_riscv.c must
# compile clean under the kernel's own RV_CFLAGS and link with ZERO
# undefined symbols.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/simi_riscv.c is a direct, unmodified copy of
# tools/simi/simi_riscv.c below its header (the same byte-identity
# convention kernel/simi_arm.c follows — plan doc §10.180). Its header
# claims "No libc dependency, same freestanding discipline as
# kernel/simi_x86.c". The ARM kernel-copy audit (§10.181) proved this
# class of claim needs enforcement, not assertion: GCC 13 on AArch64
# synthesized memcpy/memset CALLS from the M2 chain struct copies, and
# only an nm -u check saw them. This guard applies the same discipline to
# the RV64 copy: compile it with the exact RV_CFLAGS the kernel build
# uses (mirrored below) and assert (a) zero warnings and (b) ZERO
# undefined symbols. The RV64 kernel links -nostdlib and provides no
# global memcpy/memset, so any symbol at all — including a synthesized
# pair — would fail the link; the contract is strictly empty here, like
# the x86 copy and unlike the ARM copy's {memcpy, memset} allowance
# (§10.181).
#
# Needs the RISC-V cross toolchain (riscv64-unknown-elf-gcc/nm), so it is
# deliberately NOT globbed by tests/run_checks.sh — it is run by the
# riscv-guards CI job, which installs the toolchain (the same one that
# builds `make riscv-elf`).
#
# Exit: 0 pass, 1 fail, 2 abort (toolchain missing, or file missing).
# Optional args (for the teeth smoke): <src.c> <obj>.
set -u
cd "$(dirname "$0")/../../.."   # repo root

SRC="${1:-kernel/simi_riscv.c}"
OBJ="${2:-}"
if [ -z "$OBJ" ]; then
    TMPD=$(mktemp -d)
    trap 'rm -rf "$TMPD"' EXIT
    OBJ="$TMPD/simi_riscv.o"
fi

command -v riscv64-unknown-elf-gcc >/dev/null 2>&1 || { echo "ABORT: riscv64-unknown-elf-gcc not found — run on a host with the RISC-V cross toolchain (CI riscv-guards job)." >&2; exit 2; }
command -v riscv64-unknown-elf-nm  >/dev/null 2>&1 || { echo "ABORT: riscv64-unknown-elf-nm not found — run on a host with the RISC-V cross toolchain." >&2; exit 2; }
[ -f "$SRC" ] || { echo "ABORT: '$SRC' not found" >&2; exit 2; }

# RV_CFLAGS mirror (Makefile line ~282): the exact flags the kernel build
# uses for kernel/simi_riscv.c, so codegen and symbols match the real
# -nostdlib link. -Ikernel reproduces how the real compile resolves
# `#include "simi_riscv.h"` (the source lives in kernel/, so its own
# directory is searched first); the teeth smoke compiles throwaway copies
# from a temp dir, so it needs the include path spelled out — same
# header, same compile.
CFLAGS="-ffreestanding -O2 -Wall -Wextra -mcmodel=medany \
        -march=rv64gcv -mabi=lp64d -mno-relax -ffunction-sections -fdata-sections \
        -Ikernel"

out=$(riscv64-unknown-elf-gcc $CFLAGS -c "$SRC" -o "$OBJ" 2>&1)
if [ -n "$out" ]; then
    printf '%s\n' "FAIL  kernel/simi_riscv.c does not compile clean under RV_CFLAGS:" "$out"
    exit 1
fi

bad=$(riscv64-unknown-elf-nm -u "$OBJ" | awk '$1 == "U" { print $2 }')
if [ -n "$bad" ]; then
    printf '%s\n' "FAIL  kernel/simi_riscv.o has undefined symbols (the -nostdlib link would fail):" "$bad"
    exit 1
fi

echo "OK    kernel/simi_riscv.c compiles clean under RV_CFLAGS with zero undefined symbols"
echo "simi_riscv_kernel_check: done, 0 violations"
