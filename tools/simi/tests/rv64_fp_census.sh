#!/usr/bin/env bash
# tools/simi/tests/rv64_fp_census.sh — the RV64 FP/vector census gate
# (ISA doc §16 Phase 16 audit addendum, Design A; updated for Design B
# part 2).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The RV64 kernel's FP state is unmanaged (the audit: GPR-only trap frame,
# dead-code context switch, sstatus.FS never written by the kernel) and
# the mode-exact reality makes the CENSUS the load-bearing guard: under
# the S-mode boot OpenSBI leaves sstatus.FS=3 (Dirty), so an accidental
# F/D instruction in kernel C would EXECUTE silently — only the
# bare-metal M-mode boot keeps FS=Off where the dispatcher's scause=2
# case catches it. Design B part 2 adds a deliberate, contained FP user:
# fp_save_all/fp_load_all (arch/riscv/trap_riscv.S), the two-owner FS
# lazy-save plumbing; Design B part 3 adds the N-task ready queue's FP
# work (rv_fp_task_common in kernel/kernel_riscv.c — one fadd.d + one
# fmv.x.d per slice, pinned by the boot's [TASK] asserts).
# The invariant is therefore NOT "zero FP instructions" anymore — it is
# "FP instructions ONLY inside those three functions": a function-scoped
# allow-list, so an accidental fadd.d (or a stray fld anywhere outside
# the plumbing/tasks) still fails the gate.
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
#     the operand check covers f[ast]?[0-9]+ and the v-register class;
#   * the function gate tracks the current symbol (objdump's
#     `<name>:` headers), so sanctioned means literally inside
#     fp_save_all / fp_load_all / rv_fp_task_common — a helper moved,
#     renamed, or inlined elsewhere starts failing the census again.
# Validated: on all four clean kernel ELFs the only FP instructions are
# the two helpers' 66 fld/fsd/fmv/frcsr/fscsr lines plus the two task
# functions' fadd.d/fmv.x.d lines (sanctioned, 0 unexpected); 4/4
# known-bad lines (fadd.d, fld, vsetvli, fmv.d.x OUTSIDE the sanctioned
# functions) caught; the `and` + fence exclusions hold.
#
# Usage: rv64_fp_census.sh [elf...]   (default: the four kernel ELFs)
# Exit: 0 = clean (zero UNEXPECTED FP/vector instructions), 1 = violation
# found (prints the offending lines), 2 = objdump missing or no ELF
# found.
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

# The gate: every FP/vector instruction line is classified against the
# CURRENT function (tracked from objdump's `<symbol>:` headers). Lines
# inside the two sanctioned helpers count as sanctioned plumbing; any
# other FP/vector line is an UNEXPECTED violation and is printed.
CENSUS_AWK='
/^[0-9a-f]+ <[^>]+>:$/ { fn = $1; sub(/^[0-9a-f]+ </, "", fn); sub(/>:$/, "", fn); }
{ sub(/#.*/, "", $4); }
($3 ~ /^(fadd|fsub|fmul|fdiv|fsqrt|fmin|fmax|fmadd|fmsub|fnmadd|fnmsub|fcvt|fsgnj|fsgnjn|fsgnjx|fmv|fle|flt|feq|fclass|fld|flw|fsd|fsw|frcsr|fscsr|frrm|fsrm|frflags|fsflags|vsetvli|vsetivli|vsetvl|v)/ ||
 $4 ~ /(^|[, ])(f[ast]?[0-9]+|v[0-9]+)([., ]|$)/) {
    if (fn == "fp_save_all" || fn == "fp_load_all" ||
        fn == "rv_fp_task_common") { sanc++; }
    else { print "UNEXPECTED: " $0; bad++; }
}
END { print "SANCTIONED: " sanc + 0; }'

fail=0
found=0
for elf in "${elfs[@]}"; do
    [ -f "$elf" ] || {
        echo "fp-census: ABORT — $elf not found (run 'make riscv-elf' first)." >&2
        exit 2
    }
    found=$((found + 1))
    out=$("$OBJDUMP" -d "$elf" | awk -F"\t" "$CENSUS_AWK")
    unexpected=$(printf '%s\n' "$out" | grep -c '^UNEXPECTED:')
    sanctioned=$(printf '%s\n' "$out" | sed -n 's/^SANCTIONED: //p')
    [ -z "$sanctioned" ] && sanctioned=0
    if [ "$unexpected" -eq 0 ]; then
        echo "fp-census: OK — $elf: $sanctioned sanctioned FP instructions (fp_save_all/fp_load_all + rv_fp_task_common), 0 unexpected"
    else
        echo "fp-census: FAILED — $elf contains $unexpected unexpected FP/vector instruction line(s) (sanctioned: $sanctioned):" >&2
        printf '%s\n' "$out" | grep '^UNEXPECTED:' | head -10 >&2
        fail=$((fail + 1))
    fi
done

[ "$found" -gt 0 ] || {
    echo "fp-census: ABORT — no ELF files to check." >&2
    exit 2
}

exit "$fail"
