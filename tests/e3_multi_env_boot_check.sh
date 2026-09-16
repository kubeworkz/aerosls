#!/usr/bin/env bash
# tests/e3_multi_env_boot_check.sh — proves POSIX-Environments E3 on the real
# target: init, built with its `e3_envs` feature, spawns 2 tenant POSIX
# environments AFTER the system POSIX, each with its OWN ramdisk driver and a
# PRIVATE R|W storage region carved from the frame pool via SYS_SLS_ALLOC_REGION
# (charged to init's partition). Each tenant POSIX formats its blank ramdisk on
# first mount (vfs mount_aerofs_or_format) and comes up with a live rootfs.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# E3 is the multi-instance step of the POSIX-Environments roadmap: one running
# kernel hosting several independent POSIX runtimes (the IBM-PASE-per-LPAR
# shape). The whole chain that makes that real only exists at runtime — the
# per-partition contiguous region allocator (frame_pool), the SYS_SLS_ALLOC_REGION
# syscall, the partition-scoped sidecar registry (E2, so drv.ramdisk.1/.2
# coexist with the system's drv.ramdisk.0), the tenant manifest profile (no
# hardware/network caps), and the POSIX self-format path. A regression anywhere
# there shows up as a missing/failed '[INIT] E3 ...' line or as the two tenants
# colliding on one storage region. This smoke makes CI see it.
#
# The core E3 property asserted here is ISOLATION BY CONSTRUCTION: the two
# tenant environments are handed DISTINCT storage base addresses (independent
# k_alloc_region runs), so their block devices cannot alias. That, plus both
# tenants' ramdisk+POSIX pairs spawning and both reaching a live rootfs, is the
# proof that the system stood up N genuinely separate POSIX instances.
#
# Prerequisite: the E3 ISO, which is NOT the shipped image — build it with
#   make x86-iso-e3
# (init compiled --features e3_envs, packed into sidecars_e3.cpio, embedded in
# sls_operating_system_e3.iso). The shipped sls_operating_system.iso never
# spawns tenant environments, so pointing this check at it is a SKIP, not a
# pass. Also needs qemu-system-x86_64. Reads nothing else; touches only its
# temp log.
#
# GUARD-KIND: runtime (needs the built E3 ISO + QEMU).
#
# Exit: 0 if both tenant environments came up at distinct storage addresses,
# 1 if QEMU died first or a marker/assertion failed, 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E3_ISO:-sls_operating_system_e3.iso}"
LOG=/tmp/aerosls_e3_multi_env.log
WINDOW_S=120              # the tenant envs are spawned last (after the system
                          # POSIX's network boot), so give the whole chain room

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso-e3" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }

rm -f "$LOG"
# Same device model as phase5_boot_smoke / x86-run: the system POSIX still does
# its network + DEV bring-up (the tenants use neither). The tenant environments
# ride on top of an otherwise-normal Phase 5 boot.
qemu-system-x86_64 -cdrom "$ISO" -display none -m 4G -no-reboot \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -serial file:"$LOG" 2>/dev/null &
QPID=$!

# Two signals gate the assertions, polled (not a fixed sleep) so the check
# waits exactly as long as the tenants — which are scheduled LAST — need:
#   1. init's final marker: it has logged every tenant spawn attempt;
#   2. three '[POSIX] aero state=00000000' lines: the system POSIX plus BOTH
#      tenants have each brought up a live (formatted) rootfs — the strong,
#      drop-resistant proof the tenants actually booted (init's per-line
#      "…spawned" console messages can be coalesced away when three sidecars
#      flood the kernel console at once, so we do NOT gate on them).
done_marker="\[INIT\] E3: tenant environments spawned."
live_re="\[POSIX\] aero state=00000000"
booted=0
qemu_alive=1
for _i in $(seq 1 $((WINDOW_S / 2))); do
    live_n=$(grep -acE "$live_re" "$LOG" 2>/dev/null); live_n=${live_n:-0}
    if grep -aqE "$done_marker" "$LOG" 2>/dev/null && [ "$live_n" -ge 3 ]; then
        booted=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then
        qemu_alive=0
        break
    fi
    sleep 2
done
kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

if [ "$booted" -eq 0 ]; then
    live_n=$(grep -acE "$live_re" "$LOG" 2>/dev/null); live_n=${live_n:-0}
    if [ "$qemu_alive" -eq 0 ]; then
        echo "FAIL  QEMU exited before both tenant environments came up (live rootfs: $live_n/3)" >&2
    else
        echo "FAIL  within ${WINDOW_S}s: init done-marker or 3 live rootfs never seen (live rootfs: $live_n/3)" >&2
    fi
    echo "      last 40 serial lines:" >&2
    tail -40 "$LOG" >&2
    exit 1
fi

fail=0

# No allocation may have failed — every tenant region must come from the pool.
if grep -aqE "\[INIT\] E3 env [0-9]+: alloc_region FAILED" "$LOG"; then
    echo "FAIL  a tenant region allocation failed (frame pool exhausted or quota denied)" >&2
    grep -aE "alloc_region FAILED" "$LOG" >&2
    fail=1
fi
# No sidecar spawn may have failed.
if grep -aqE "\[INIT\] E3 env [0-9]+: .* spawn FAILED" "$LOG"; then
    echo "FAIL  a tenant sidecar spawn failed" >&2
    grep -aE "spawn FAILED" "$LOG" >&2
    fail=1
fi

# That both ramdisks and both tenant POSIX sidecars came up is proven below by
# the distinct-storage lines (ramdisks) and the 3-live-rootfs count (POSIX) —
# stronger than, and not subject to the console-coalescing that can drop, the
# per-env "…spawned" lines init logs.

# The core E3 property: the two tenants got DISTINCT storage base addresses
# (independent k_alloc_region runs — their block devices cannot alias). Parse
# the 'storage @ 0x...' address out of each env's region line.
addr_of() {
    grep -aoE "\[INIT\] E3 env $1: drv\.ramdisk\.$1 storage @ 0x[0-9a-fA-F]+" "$LOG" \
        | head -1 | grep -aoE "0x[0-9a-fA-F]+"
}
s1="$(addr_of 1)"
s2="$(addr_of 2)"
if [ -z "$s1" ] || [ -z "$s2" ]; then
    echo "FAIL  could not read both tenant storage addresses (env1='$s1' env2='$s2')" >&2
    grep -aE "\[INIT\] E3 env [0-9]+: .* storage @" "$LOG" >&2
    fail=1
elif [ "$s1" = "$s2" ]; then
    echo "FAIL  the two tenant environments share storage $s1 — regions are NOT isolated" >&2
    fail=1
else
    echo "ok:   tenant storage regions are distinct (env1=$s1, env2=$s2)"
fi

# Secondary tooth: all three POSIX instances (system + 2 tenants) must reach a
# LIVE rootfs. For the tenants a live rootfs proves mount_aerofs_or_format
# formatted the blank k_alloc_region region successfully (a failed format/mount
# never reaches this diagnostic). The sidecar's klog prints the u32 state as
# 8-digit zero-padded uppercase hex, so Live (0) is exactly 'state=00000000'
# (matching a bare 'state=0' would also catch stale codes like 00000003).
live_n=$(grep -acE "\[POSIX\] aero state=00000000" "$LOG" 2>/dev/null); live_n=${live_n:-0}
if [ "$live_n" -lt 3 ]; then
    echo "FAIL  only $live_n/3 POSIX instances reported a live rootfs (tenant self-format failed?)" >&2
    grep -aE "\[POSIX\] aero state=" "$LOG" >&2
    fail=1
else
    echo "ok:   $live_n POSIX instances came up with a live rootfs (system + 2 tenants)"
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL  E3 multi-instance boot check failed" >&2
    echo "      init E3 lines seen:" >&2
    grep -aE "\[INIT\] E3" "$LOG" >&2
    exit 1
fi

echo "OK: two tenant POSIX environments booted with distinct private ramdisks (E3 multi-instance)"
exit 0
