#!/usr/bin/env bash
# tools/simi/tests/rv64_fp_gate_smoke.sh — teeth for the RV64 FP/vector
# gates (ISA doc §16 Phase 16 audit addendum, Design A).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The RV64 FP audit found the kernel's FP state is unmanaged and the
# mode-exact reality makes the CENSUS the load-bearing guard (S-mode
# boots under OpenSBI with sstatus.FS=3 — an FP instruction would
# EXECUTE silently; only the bare-metal M-mode boot keeps FS=Off where
# the dispatcher's scause=2 case traps). This smoke proves both
# machine-checkable halves have teeth, the arm64 pattern:
#   * tooth 1 (census): inject a real fadd.d into kernel_riscv.c,
#     rebuild, and assert rv64_fp_census.sh FAILS on the broken ELF.
#   * tooth 2 (backstop): boot the broken M-MODE kernel and assert the
#     FP access TRAPS with the specific diagnostic — the fadd.d fires
#     after riscv_trap_init armed mtvec, so the dispatcher's scause=2
#     case prints "[TRAP] illegal instruction (scause=2),
#     sstatus.FS=0 ... FP-free by design ... halting hart", and the
#     boot never reaches the SIMI smoke. The S-mode build is NOT
#     asserted here: under OpenSBI FS=3 means the same instruction
#     executes silently — the census is the only guard there, which is
#     exactly why the census tooth must fire.
# Tooth 0 (the clean kernels pass the census) proves the teeth are
# meaningful; the restore step leaves the tree and ELFs clean.
#
# Deliberately NOT globbed by tests/run_guard_smokes.sh (needs the
# riscv64-unknown-elf toolchain AND qemu-system-riscv64): the
# riscv-guards CI job runs it.
#
# Exit: 0 if all teeth fired, 1 otherwise, 2 if a required tool is
# missing.
set -u
cd "$(dirname "$0")/../../.."   # repo root

for t in riscv64-unknown-elf-gcc riscv64-unknown-elf-objdump qemu-system-riscv64; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ABORT: $t not found — run on a host with the riscv64 toolchain + qemu (CI riscv-guards job)." >&2
        exit 2
    }
done

CENSUS=tools/simi/tests/rv64_fp_census.sh
TMP=/tmp/rv64_fp_smoke
mkdir -p "$TMP"
LOG="$TMP/m_boot.log"

# The smoke modifies kernel/kernel_riscv.c in place; save the original
# and guarantee its restore even if the script dies mid-run (the
# explicit restore step below also re-asserts it). No git dependency:
# the backup travels with the script across WSL/Git Bash/CI alike.
cp kernel/kernel_riscv.c "$TMP/save_kernel_riscv.c"
trap 'cp "$TMP/save_kernel_riscv.c" kernel/kernel_riscv.c 2>/dev/null || true' EXIT

pass=0
fail=0

echo "RV64 FP/vector gate smoke"
echo "========================="
echo

# ── tooth 0: the CLEAN kernels must pass the census (sanity — proves
# ── the gate isn't trivially broken, so the teeth below are meaningful).
echo "tooth 0: the clean kernels pass the zero-FP census"
if make riscv-elf >/dev/null 2>&1 && bash "$CENSUS" >/dev/null 2>&1; then
    echo "  PASS  clean ELFs census clean"
    pass=$((pass + 1))
else
    echo "  FAIL  clean ELFs failed the census — the gate itself is broken"
    fail=$((fail + 1))
fi
echo

# ── tooth 1: inject a real FP instruction into kernel_riscv.c (after
# ── riscv_trap_init arms the vector table, so the M-mode trap tooth
# ── below routes through the dispatcher), rebuild, and assert the
# ── CENSUS catches it.
echo "tooth 1: an FP instruction fails the census"
sed -i "/riscv_trap_init(&g_hart0_data, trap_stack_top);/a\\
    asm volatile(\"fadd.d f10, f10, f11\" ::: \"memory\");" kernel/kernel_riscv.c
if make riscv-elf >/dev/null 2>&1 && bash "$CENSUS" >/dev/null 2>&1; then
    echo "  FAIL  the injected fadd.d slipped through — the census is blind"
    fail=$((fail + 1))
else
    echo "  PASS  the injected fadd.d caught by the census"
    pass=$((pass + 1))
fi
echo

# ── tooth 2: boot the broken M-MODE kernel and assert the FPEN-class
# ── backstop: FS=Off (reset, bare metal) makes the fadd.d trap
# ── scause=2, the dispatcher prints the specific FP-free diagnostic,
# ── and the hart halts — the boot never reaches the SIMI smoke.
echo "tooth 2: the broken M-mode kernel TRAPS on the FP access (FS=Off)"
rm -f "$LOG"
timeout 30 qemu-system-riscv64 -M virt -bios none -kernel sls_riscv_kernel_m.elf \
    -nographic -serial "file:$LOG" -no-reboot >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 124 ]; then
    echo "  FAIL  expected the trap halt (rc=124), got rc=$rc"
    fail=$((fail + 1))
elif grep -q "\[TRAP\] illegal instruction (scause=2), sstatus.FS=0" "$LOG" \
     && grep -q "FP-free by design (rv64_fp_census gate)" "$LOG" \
     && ! grep -q "\[SIMI\] RV64 boot smoke test" "$LOG"; then
    echo "  PASS  the fadd.d trapped with the FP-free diagnostic (rc=124)"
    pass=$((pass + 1))
else
    echo "  FAIL  the FP access did not trap with the specific message"
    tail -5 "$LOG" >&2
    fail=$((fail + 1))
fi
echo

# ── restore: revert the injected kernel from the backup and rebuild,
# ── so the tree and the on-disk ELFs are clean again (the trap above
# ── guarantees it even on failure).
echo "restore: revert the injected kernel and rebuild"
if cp "$TMP/save_kernel_riscv.c" kernel/kernel_riscv.c \
   && make riscv-elf >/dev/null 2>&1 \
   && bash "$CENSUS" >/dev/null 2>&1; then
    echo "  PASS  restored clean kernels pass the census"
    pass=$((pass + 1))
else
    echo "  FAIL  restore failed — the tree is dirty or the rebuild broke"
    fail=$((fail + 1))
fi
echo

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
