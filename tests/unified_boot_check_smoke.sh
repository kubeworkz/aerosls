#!/usr/bin/env bash
# tests/unified_boot_check_smoke.sh — proves tests/unified_boot_check.sh has
# teeth: pointed at a boot that does NOT have the E1 property, the guard must
# FAIL (not pass quietly, and not hang).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# unified_boot_check.sh asserts a property of ONE boot mode (POSIX-Environments
# E1's unified boot: the Ring-0 control plane and the Ring-3 sidecar world
# sharing the CPU in a single boot). Every assertion in it is a grep for a line
# the kernel or init prints — and a grep for a line that is never printed is
# indistinguishable from a grep that was never evaluated. The failure mode this
# smoke closes is the one that matters most here: a guard whose patterns drift
# out of the sources (a reworded log line, a renamed print) keeps "passing" only
# as long as nobody checks that it can still fail.
#
# The sabotage is the guard's own input: UNIFIED_BOOT_ENTRY=2 boots grub's
# kernel-only entry instead of the unified one. That boot has the kernel HTTP
# server and the shell but NO unified=1 on the command line, NO control-plane
# pseudo-process, NO init sidecar and no heartbeats — every E1 assertion must
# therefore fail, and the guard must say which. Then the RESTORE step runs the
# guard unmodified and requires it to PASS, so this smoke also proves the guard
# is not simply failing everything (a tooth that fires on both inputs proves
# nothing about either), and leaves no footprint behind.
#
# Needs the built ISO + QEMU, so it is on run_source_smokes.sh's build-needed
# exclusion list and runs on a build host via run_guard_smokes.sh (and in CI's
# kernel-guards job) — same as the phase5 boot smokes.
#
# Exit: 0 when the guard failed on the sabotaged boot AND passed on the real
# one, 1 otherwise, 2 on a missing prerequisite.
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

# ── The tooth: the guard must fail on a boot that lacks the property ────────
# The window is short on purpose: the sabotaged boot fails at the guard's
# command-line check (it names the wrong boot as soon as the kernel prints it),
# so a long wait would only prove the guard is slow, not that it is right.
echo "TOOTH  unified_boot_check.sh pointed at the kernel-only boot (grub entry 2)"
out="$(UNIFIED_BOOT_ENTRY=2 UNIFIED_BOOT_WINDOW_S=45 bash tests/unified_boot_check.sh 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -qF "FAILED:"; then
    echo "TOOTH OK   guard failed (rc=1) on a boot without unified=1:"
    printf '%s\n' "$out" | grep -F "FAILED:" | head -3 | sed 's/^/           /'
else
    echo "TOOTH FAIL guard rc=$rc on the kernel-only boot, expected 1 with a FAILED: line" >&2
    printf '%s\n' "$out" | sed 's/^/           /' >&2
    fails=$((fails + 1))
fi

# ── RESTORE: the real input must still pass ────────────────────────────────
echo "RESTORE unified_boot_check.sh on the unified boot (grub entry 3)"
out="$(bash tests/unified_boot_check.sh 2>&1)"
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
echo "OK: the unified-boot guard fails on a boot without the E1 property and passes on it (teeth proven)"
exit 0
