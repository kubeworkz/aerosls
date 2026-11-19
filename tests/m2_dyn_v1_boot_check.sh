#!/usr/bin/env bash
# tests/m2_dyn_v1_boot_check.sh — M2d-v1's live tooth: a VERSION-1 manifest
# carrying a PT_INTERP image must be refused BY NAME, not entered at e_entry
# with no loader.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/cap.c's refusal is otherwise source-pinned only: O10 holds the
# branch and its message, but nothing on the boot path ever hands the kernel
# a version-1 manifest around a dynamic image, so the guard could stay green
# while the refusal was a dead branch. This check boots the M2d ISO, reads
# the serial log, and asserts the kernel's own words — the named CAP_EINVAL
# payload that names the version and the stray loader path — plus the
# sidecar-side confirmation that the probe saw CAP_EINVAL and that the
# version-2 control child (M2DYN-OK) still ran in the same boot.
#
# The probe is a boot-generation one-shot, like M2a/M2d: it runs once, in the
# sidecar's gen-0 block, after the accepted dynamic spawn.
#
# GUARD-KIND: runtime (needs the built E3 ISO with M2A_INTERP=1 + QEMU).
#
# Usage:
#   M2DYN_ISO=/tmp/m2dyn_v1.iso bash tests/m2_dyn_v1_boot_check.sh
# Build that ISO with:
#   make x86-iso-e3 M2A_INTERP=1 E3_INIT_FEATURES=e3_envs,e7_gate \
#        X86_E3_ISO=/tmp/m2dyn_v1.iso SIDECAR_E3_CPIO=/tmp/m2dyn_v1.cpio
set -u
cd "$(dirname "$0")/.." || exit 1

ISO="${M2DYN_ISO:-/tmp/m2dyn_v1.iso}"
if [ ! -f "$ISO" ]; then
    echo "ABORT: $ISO not found — build the M2d ISO first" >&2
    echo "       (make x86-iso-e3 M2A_INTERP=1 E3_INIT_FEATURES=e3_envs,e7_gate ...)" >&2
    exit 2
fi

W="$(mktemp -d)"
LOG="$W/serial.log"
trap 'rm -rf "$W"' EXIT

# Same boot shape as the M2d evidence run: headless, 4 GiB, four TCG vCPUs,
# no disk, bounded wall clock. QEMU exiting 124 here is the bound doing its
# job, not a failure — the assertions are on the log.
timeout "${M2DYN_BOOT_SECS:-90}" qemu-system-x86_64 -cdrom "$ISO" -m 4G -smp 4 \
    -accel "${M2DYN_ACCEL:-tcg}" -display none -monitor none -no-reboot \
    -boot d -serial "file:$LOG" >"$W/qemu.out" 2>"$W/qemu.err" || true

fail=0
has() {
    if grep -aqF "$1" "$LOG"; then
        echo "ok:   $2"
    else
        echo "FAILED: $2" >&2
        fail=1
    fi
}
absent() {
    if grep -aqF "$1" "$LOG"; then
        echo "FAILED: $2" >&2
        fail=1
    else
        echo "ok:   $2"
    fi
}

# 1. The kernel's refusal, in its own words. This is the named line M2d-v1
#    exists to observe: version 1 and the image's own PT_INTERP path.
has "[CS] CAP_EINVAL: PT_INTERP requires a version-2 manifest (version=1, path='/lib/ld-musl-x86_64.so.1')" \
    "the kernel refused the version-1 PT_INTERP manifest by name (version=1, path='/lib/ld-musl-x86_64.so.1')"

# 2. The sidecar saw that refusal as CAP_EINVAL (-1 — klog prints it as
#    0xFFFFFFFF), not as some other create error.
has "[M2D] v1 CAP_EINVAL rc=FFFFFFFF" \
    "the probe's create returned CAP_EINVAL (not an unrelated refusal)"

# 3. The negative probe must not be a silent admission.
absent "[M2D] v1 ADMITTED" \
    "no version-1 manifest was admitted"

# 4. The accepted version-2 route still runs in the same boot: the probe
#    must not disturb the sibling it follows.
has "[M2D] hello: spawned" \
    "the version-2 dynamic spawn still happened"
has "M2DYN-OK" \
    "the accepted dynamic child still reached its own marker"

if [ "$fail" -ne 0 ]; then
    echo "boot log: $LOG (kept for inspection)" >&2
    trap - EXIT
    echo "FAILED: M2d-v1 — the version-1 PT_INTERP refusal was not observed (M2d)"
    exit 1
fi

echo "OK: a version-1 manifest carrying PT_INTERP was refused by name in the live kernel — CAP_EINVAL named version=1 and the loader path, the probe saw -1, no child was admitted, and the version-2 child still ran (M2d-v1)"
exit 0
