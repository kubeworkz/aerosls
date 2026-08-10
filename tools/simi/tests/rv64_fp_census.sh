#!/usr/bin/env bash
# tools/simi/tests/rv64_fp_census.sh — the RV64 FP/vector census gate
# (ISA doc §16 Phase 16 audit addendum, Design A).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The RV64 kernel's FP state is entirely unmanaged (the audit: GPR-only
# trap frame, dead-code context switch, sstatus.FS never written by the
# kernel) and the mode-exact reality makes the CENSUS the load-bearing
# guard: under the S-mode boot OpenSBI leaves sstatus.FS=3 (Dirty), so
# an accidental F/D instruction would EXECUTE silently — only the
# bare-metal M-mode boot keeps FS=Off where the dispatcher's scause=2
# case traps. Whether or not the trap exists, zero FP/vector instructions
# in the image is the invariant that must hold. This script is that
# invariant as a committed gate: it disassembles the linked kernel ELF(s)
# and fails if ANY FP/vector instruction appears.
#
# ─── What it checks ────────────────────────────────────────────────────────
# The audit's methodology lessons are baked in:
#   * the toolchain is riscv64-unknown-elf-objdump, NOT -linux-gnu- (a
#     wrong name silently produces empty output — a blind gate);
#   * the check is scoped to objdump's tab-separated MNEMONIC + OPERAND
#     columns, with #-comments stripped — the raw 16/32-bit encoding
#     column false-positives (compressed sd encodings like f016) and the
#     disassembler's comment hex (# f80 <_start-...>) both match naive
#     \bf[0-9]+\b;
#   * fence (the memory fence) is NOT FP and is excluded;
#   * RISC-V objdump prints FP registers by ABI name (fa0/ft0/fs0), so
#     the operand check covers f[ast]?[0-9]+ and the v-register class.
# Validated: 0 matches on all four clean kernel ELFs; 4/4 known-bad lines
# (fadd.d, fld, vsetvli, fmv.d.x) caught; the `and` + fence exclusions
# hold.
#
# Usage: rv64_fp_census.sh [elf...]   (default: the four kernel ELFs)
# Exit: 0 = clean (zero FP/vector instructions), 1 = violation found
# (prints the offending lines), 2 = objdump missing or no ELF found.
set -u
cd "$(dirname "$0")/../../.."   # repo root

OBJDUMP=riscv64-unknown-elf-objdump
DEFAULT_ELFS="sls_riscv_kernel.elf sls_riscv_kernel_m.elf sls_riscv_kernel_echo.elf sls_riscv_kernel_echo_m.elf"

command -v "$OBJDUMP" >/dev/null 2>&1 || {
    echo "ABORT: $OBJDUMP not found — run on a host with the riscv64-unknown-elf toolchain (CI riscv-guards job)." >&2
    exit 2
}

if [ "$#" -gt 0 ]; then
    elfs=("$@")
else
    elfs=($DEFAULT_ELFS)
fi

CENSUS_AWK='
{ sub(/#.*/, "", $4); }
$3 ~ /^(fadd|fsub|fmul|fdiv|fsqrt|fmin|fmax|fmadd|fmsub|fnmadd|fnmsub|fcvt|fsgnj|fsgnjn|fsgnjx|fmv|fle|flt|feq|fclass|fld|flw|fsd|fsw|frcsr|fscsr|frrm|fsrm|frflags|fsflags|vsetvli|vsetivli|vsetvl|v)/ ||
$4 ~ /(^|[, ])(f[ast]?[0-9]+|v[0-9]+)([., ]|$)/   { print }'

fail=0
found=0
for elf in "${elfs[@]}"; do
    [ -f "$elf" ] || {
        echo "fp-census: ABORT — $elf not found (run 'make riscv-elf' first)." >&2
        exit 2
    }
    found=$((found + 1))
    n=$("$OBJDUMP" -d "$elf" | awk -F"\t" "$CENSUS_AWK" | wc -l)
    if [ "$n" -eq 0 ]; then
        echo "fp-census: OK — $elf contains zero FP/vector instructions"
    else
        echo "fp-census: FAILED — $elf contains $n FP/vector instruction line(s):" >&2
        "$OBJDUMP" -d "$elf" | awk -F"\t" "$CENSUS_AWK" | head -10 >&2
        fail=$((fail + 1))
    fi
done

[ "$found" -gt 0 ] || {
    echo "fp-census: ABORT — no ELF files to check." >&2
    exit 2
}

exit "$fail"
