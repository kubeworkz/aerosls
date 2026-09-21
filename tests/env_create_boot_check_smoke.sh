#!/usr/bin/env bash
# tests/env_create_boot_check_smoke.sh — proves tests/env_create_boot_check.sh
# has teeth: pointed at inputs that do NOT have the E4 property, the guard must
# FAIL (not pass quietly, and not hang).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# env_create_boot_check.sh asserts a property of ONE boot (POSIX-Environments
# E4: an environment placed by HTTP into an HTTP-created partition, carrying
# that partition, charged to it, with placements the kernel must refuse
# refused). Every assertion in it is either an HTTP response field or a serial
# line — and an assertion that is never really evaluated is indistinguishable
# from one that holds. The failure this smoke closes is the one that matters
# most here: a refusal arm that "passes" because the API answered `ok:false`
# for an unrelated reason, or a guard whose patterns drifted out of the
# sources and now match nothing.
#
# Three arms, two of them teeth:
#
#   1. TOOTH (wrong boot)  — E4_BOOT_ENTRY=2 boots grub's kernel-only entry,
#      which has the kernel HTTP server but NO unified control plane and NO
#      init, so there is no environment manager to drive: the guard must fail,
#      and the guard's own contradiction check names it early (the window is
#      short on purpose — the verdict comes from the command line, not from
#      waiting out a dead premise).
#
#   2. TOOTH (withheld refusal) — E4_TOOTH=skip-pause runs the guard on the
#      REAL unified boot but does not pause the second partition before asking
#      for an environment in it. That is the same input the kernel's placement
#      check produces when it is removed: the create is ALLOWED, so the
#      refusal arm must go red. A guard whose refusal assertions were vacuous
#      (grepping for a string that is there anyway, or accepting any ok:false)
#      passes this arm, and the smoke fails it for that.
#
#   3. RESTORE — the guard unmodified on the unified boot must pass, so the
#      smoke also proves the guard is not simply failing everything (a tooth
#      that fires on both inputs proves nothing about either) and leaves no
#      footprint behind.
#
# The SOURCE control that matches arm 2 — deleting the paused-target check in
# `sys_sls_alloc_region` and booting that image — is not automated here for
# the same reason tests/unified_boot_check_smoke.sh does not build a
# yield-less ISO: it needs a second kernel build. It was run by hand (the guard
# went red on the refusal arm, naming the missing
# `[ALLOC_REGION] CAP_EPERM: target partition N is paused` line and `the
# refused partition was populated anyway (1344 frames, 0 processes)`) and is
# recorded in roadmap §7.2; the guard honours E4_ISO so that control can be
# repeated against a sabotaged image without touching the shipped one. The same
# deletion in `cap_create_sidecar_in` no longer turns this guard red: the
# region gate refuses before the sidecars are asked for, which is exactly why
# a refused placement now allocates nothing.
#
# The CHARGING half has a source control of the same shape: reverting
# `sys_sls_alloc_region` to charge the caller (`caller->partition_id` instead
# of the resolved target) turns the guard red on the two frame assertions —
# `partition 1's frame_usage grew by only 176 frames` and `the creator's
# frame_usage grew by 1360 frames` — which is the pre-fix measurement §7.2
# recorded, now reproduced from a rebuilt image (also run by hand, same reason).
#
# Needs the built ISO + QEMU, so it is on run_source_smokes.sh's build-needed
# exclusion list and runs on a build host via run_guard_smokes.sh (and in CI's
# kernel-guards job) — same as the phase5 boot smokes.
#
# Exit: 0 when every tooth fired and the real input still passed, 1 otherwise,
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

[ -f sls_operating_system.iso ] || {
    echo "ABORT: sls_operating_system.iso missing — build it with: make x86-iso" >&2
    exit 2
}
command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "ABORT: qemu-system-x86_64 not installed" >&2
    exit 2
}

fails=0

# ── Tooth 1: a boot with no environment manager must fail the guard ─────────
echo "TOOTH  env_create_boot_check.sh pointed at the kernel-only boot (grub entry 2)"
out="$(E4_BOOT_ENTRY=2 E4_WINDOW_S=45 bash tests/env_create_boot_check.sh 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "FAILED:"; then
    echo "TOOTH OK   guard failed (rc=1) on a boot with no unified control plane:"
    printf '%s\n' "$out" | grep -F "FAILED:" | head -3 | sed 's/^/           /'
else
    echo "TOOTH FAIL guard rc=$rc on the kernel-only boot, expected 1 with a FAILED: line" >&2
    printf '%s\n' "$out" | sed 's/^/           /' >&2
    fails=$((fails + 1))
fi

# ── Tooth 2: the refusal arm must bite when the refusal is withheld ─────────
echo "TOOTH  env_create_boot_check.sh with E4_TOOTH=skip-pause (the placement is allowed)"
out="$(E4_TOOTH=skip-pause bash tests/env_create_boot_check.sh 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "was NOT refused"; then
    echo "TOOTH OK   guard failed (rc=1) when the refused placement was allowed instead:"
    printf '%s\n' "$out" | grep -F "FAILED:" | head -3 | sed 's/^/           /'
else
    echo "TOOTH FAIL guard rc=$rc with E4_TOOTH=skip-pause, expected 1 naming a create that was NOT refused" >&2
    printf '%s\n' "$out" | sed 's/^/           /' >&2
    fails=$((fails + 1))
fi

# ── RESTORE: the real input must still pass ────────────────────────────────
echo "RESTORE env_create_boot_check.sh on the unified boot (grub entry 3)"
out="$(bash tests/env_create_boot_check.sh 2>&1)"
rc=$?
if [ "$rc" -eq 0 ]; then
    echo "RESTORE OK guard passed on the unified boot"
    printf '%s\n' "$out" | grep -F "ok:" | sed 's/^/           /'
else
    echo "RESTORE FAIL guard rc=$rc on the unified boot, expected 0" >&2
    printf '%s\n' "$out" | sed 's/^/           /' >&2
    fails=$((fails + 1))
fi

[ "$fails" -eq 0 ] || { echo "FAIL  $fails tooth/restore assertion(s) failed" >&2; exit 1; }
echo "OK: the environment-create guard fails when the placement rules are not enforced and passes when they are (teeth proven)"
exit 0
