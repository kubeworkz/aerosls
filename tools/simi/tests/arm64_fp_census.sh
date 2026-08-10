#!/usr/bin/env bash
# tools/simi/tests/arm64_fp_census.sh — the M5 FP/SIMD census gate
# (plan doc §10.198, leg 2: the shipped ELF contains zero FP/SIMD
# instructions).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The arm64 kernel's zero-FP claim rests on three machine-checked legs:
# (1) -mgeneral-regs-only constrains the C codegen, (2) the LINKED ELF's
# disassembly contains zero FP/SIMD instructions (closing the
# hand-written-asm hole the flag cannot see), and (3) CPACR_EL1.FPEN=0
# makes any violation a loud trap rather than silent corruption. This
# script is leg 2 made a committed gate: it disassembles the kernel ELF
# and fails if ANY FP/SIMD instruction appears — the same shape as the
# M2-gate freestanding compile, a machine check rather than an argument.
#
# ─── What it checks ────────────────────────────────────────────────────────
# objdump -d emits "ADDR:\tWORD\tMNEMONIC\tOPERANDS" (tab-separated), so
# the census is scoped to the mnemonic and operand COLUMNS — the raw
# 8-hex-char encoding word (e.g. 0xd5384242) must not count (a hex token
# like d5384242 trivially matches \bd[0-9]+\b). Two rules, OR'd:
#   * mnemonic starts with the FP family (fmov/fadd/fsub/fmul/fdiv/fcmp/
#     fabs/fneg/fcvt*/frint*), and
#   * the operand column carries a register of the FP/vector class
#     (d/s/v/q/h + number, allowing the vector-suffix forms like v0.4s).
# Branch-target hex addresses and #0x... immediates in the operand column
# cannot false-positive: a d in the middle of a hex token has no word
# boundary before it, and objdump prints immediates as #0x... or decimal.
#
# Usage: arm64_fp_census.sh [elf-path]     (default: sls_arm64_kernel.elf)
# Exit: 0 = clean (zero FP/SIMD instructions), 1 = violation found
# (prints the offending lines), 2 = objdump missing or ELF not found.
set -u
cd "$(dirname "$0")/../../.."   # repo root

ELF="${1:-sls_arm64_kernel.elf}"

command -v aarch64-linux-gnu-objdump >/dev/null 2>&1 || {
    echo "ABORT: aarch64-linux-gnu-objdump not found — run on a host with the arm64 cross toolchain (CI arm64-guards job)." >&2
    exit 2
}
[ -f "$ELF" ] || {
    echo "ABORT: $ELF not found — run 'make arm64-elf' first." >&2
    exit 2
}

CENSUS_AWK='
$3 ~ /^(fmov|fadd|fsub|fmul|fdiv|fcmp|fabs|fneg|fcvt|frint)/ ||
$4 ~ /(^|[, ])[dsvqh][0-9]+([., ]|$)/   { print }'

n=$(aarch64-linux-gnu-objdump -d "$ELF" | awk -F"\t" "$CENSUS_AWK" | wc -l)

if [ "$n" -eq 0 ]; then
    echo "fp-census: OK — $ELF contains zero FP/SIMD instructions"
    exit 0
fi

echo "fp-census: FAILED — $ELF contains $n FP/SIMD instruction line(s):" >&2
aarch64-linux-gnu-objdump -d "$ELF" | awk -F"\t" "$CENSUS_AWK" | head -10 >&2
exit 1
