#!/usr/bin/env bash
# tools/simi/tests/arm64_fp_gate_smoke.sh — teeth for the M5 FP/SIMD
# gates (plan doc §10.198, as built §10.199).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The arm64 kernel's zero-FP claim rests on three machine-checked legs.
# The census gate (arm64_fp_census.sh) is leg 2; this smoke proves BOTH
# machine-checkable legs have teeth, the kernel-copy teeth pattern:
#   * leg 2 (image census): inject a real SIMD instruction (fmov d0, xzr)
#     into kernel_arm64.c, rebuild, and assert arm64_fp_census.sh FAILS.
#   * leg 3 (FPEN trap): boot the broken kernel under the CI config and
#     assert the SIMD access TRAPS — the probe's "before" line prints,
#     the "after" line never does, and qemu times out (rc=124) at the
#     EL1h sync stub. The backstop is loud, never silent corruption.
# Tooth 0 (the clean kernel passes the census) proves the teeth are
# meaningful, and the restore step leaves the tree and ELF clean.
#
# Deliberately NOT globbed by tests/run_guard_smokes.sh (needs the arm64
# cross toolchain AND qemu-system-aarch64): the arm64-guards CI job runs
# it, like arm64_kernel_copy_smoke.sh.
#
# Exit: 0 if all teeth fired, 1 otherwise, 2 if a required tool is
# missing.
set -u
cd "$(dirname "$0")/../../.."   # repo root

for t in aarch64-linux-gnu-gcc aarch64-linux-gnu-objdump qemu-system-aarch64; do
    command -v "$t" >/dev/null 2>&1 || {
        echo "ABORT: $t not found — run on a host with the arm64 cross toolchain + qemu (CI arm64-guards job)." >&2
        exit 2
    }
done

CENSUS=tools/simi/tests/arm64_fp_census.sh
TMP=/tmp/arm64_fp_smoke
mkdir -p "$TMP"
LOG="$TMP/boot.log"
QEMU=(qemu-system-aarch64 -M virt,virtualization=on -cpu cortex-a53 -m 1G \
      -kernel sls_arm64_kernel.elf -nographic -serial "file:$LOG" -no-reboot)

# The smoke modifies kernel_arm64.c in place; save the original and
# guarantee its restore even if the script dies mid-run (the explicit
# restore step below also re-asserts it). No git dependency: the backup
# travels with the script across WSL/Git Bash/CI alike.
cp kernel/kernel_arm64.c "$TMP/save_kernel_arm64.c"
trap 'cp "$TMP/save_kernel_arm64.c" kernel/kernel_arm64.c 2>/dev/null || true' EXIT

pass=0
fail=0

echo "arm64 FP/SIMD gate smoke"
echo "========================"
echo

# ── tooth 0: the CLEAN kernel must pass the census (sanity — proves the
# ── gate isn't trivially broken, so the teeth below are meaningful).
echo "tooth 0: the clean kernel passes the zero-SIMD census"
if make arm64-elf >/dev/null 2>&1 && bash "$CENSUS" >/dev/null 2>&1; then
    echo "  PASS  clean ELF census clean"
    pass=$((pass + 1))
else
    echo "  FAIL  clean ELF failed the census — the gate itself is broken"
    fail=$((fail + 1))
fi
echo

# ── tooth 1: inject a real SIMD instruction into kernel_arm64.c (after
# ── the arm_timer_init() call), rebuild, and assert the CENSUS catches
# ── it — leg 2's teeth. The fmov is deliberately unreachable-looking
# ── (it traps before any entry), but it is REAL code in the image: a
# ── census that misses it is blind.
echo "tooth 1: a SIMD instruction fails the census"
sed -i "/arm_timer_init();/a\\
    uart_puts(\"[TEMP] before SIMD access\");\\
    asm volatile(\"fmov d0, xzr\" ::: \"memory\");\\
    uart_puts(\"[TEMP] after SIMD access\");" kernel/kernel_arm64.c
if make arm64-elf >/dev/null 2>&1 && bash "$CENSUS" >/dev/null 2>&1; then
    echo "  FAIL  the injected fmov slipped through — the census is blind"
    fail=$((fail + 1))
else
    echo "  PASS  the injected fmov caught by the census"
    pass=$((pass + 1))
fi
echo

# ── tooth 2: boot the broken kernel and assert the FPEN trap — leg 3's
# ── teeth. With CPACR_EL1.FPEN=0 (reset, never written), the fmov traps
# ── at the EL1h sync vector (the stub's wfi): the log shows the "before"
# ── probe line, never the "after", and qemu times out (rc=124). A trap
# ── that went blind would print "after" or exit cleanly — either is a
# ── silent-corruption failure.
echo "tooth 2: the broken kernel TRAPS on the SIMD access"
rm -f "$LOG"
timeout 20 "${QEMU[@]}" >/dev/null 2>&1
rc=$?
if [ "$rc" -ne 124 ]; then
    echo "  FAIL  expected the FPEN trap hang (rc=124), got rc=$rc"
    fail=$((fail + 1))
elif grep -q "\[TEMP\] before SIMD access" "$LOG" \
     && ! grep -q "\[TEMP\] after SIMD access" "$LOG"; then
    echo "  PASS  the fmov trapped before executing (no 'after' print, rc=124)"
    pass=$((pass + 1))
else
    echo "  FAIL  the SIMD access executed — the FPEN backstop is blind"
    tail -5 "$LOG" >&2
    fail=$((fail + 1))
fi
echo

# ── restore: the smoke modified the working tree — revert the injected
# ── kernel from the backup and rebuild, so the tree and the on-disk ELF
# ── are clean again (the trap above guarantees it even on failure).
echo "restore: revert the injected kernel and rebuild"
if cp "$TMP/save_kernel_arm64.c" kernel/kernel_arm64.c \
   && make arm64-elf >/dev/null 2>&1 \
   && bash "$CENSUS" >/dev/null 2>&1; then
    echo "  PASS  restored clean kernel passes the census"
    pass=$((pass + 1))
else
    echo "  FAIL  restore failed — the tree is dirty or the rebuild broke"
    fail=$((fail + 1))
fi
echo

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
