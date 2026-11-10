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
# ─── Why this guard was red on main for a week, and what closed it ─────────
# Not the boot: the machine was reporting the property all along. Both tenants
# emitted their three boot diagnostics (loop exit0 / rc open err / aero state)
# INSIDE the irqtest's serial-loopback window, and the kernel's loopback
# interlock DISCARDED every byte written while the probe owned the port — so
# the evidence never reached the log, and this check failed on a machine that
# had reported exactly what it asserts. Measured with a kernel-side trace ring:
# 8 of that boot's 94 SYS_SLS_SERIAL_WRITE calls ran inside the window, and
# they were precisely the two tenants' batches.
#
# Closed on the product side (kernel/kernel_io.c): output during a window is
# now DEFERRED and replayed when the port is provably free again, so no writer
# loses its line to another's demo. Closed on this side: the gate is the three
# live-rootfs lines only (see the gate comment for why init's marker is
# reported, not gating). The teeth are in
# tests/e3_multi_env_boot_check_smoke.sh, which also pins the negative control
# this guard's own comment used to get wrong: pointed at the SHIPPED ISO (no
# tenant spawns) this check must FAIL — it is not a SKIP.
#
# ─── Deferral is bounded, and the bound is its own clause ───────────────────
# Deferral makes the evidence drop-proof only up to the kernel's ring
# (SERIAL_LB_DEFER_CAP). One E3 boot overflowed the old 2048-byte ring and the
# kernel said so on the wire — `[serial] loopback window overflow: 0x45c
# byte(s) dropped` — and this check reported that boot as `live rootfs: 2/3`,
# i.e. as a boot failure the machine had never reported. The ring is 16 KiB now
# (16x the observed loss, and the 16 KiB boot replayed both tenants' batches
# untouched), and ANY overflow line is treated as its own named failure —
# serial evidence lost — BEFORE the count is read as an outcome. A dropped byte
# makes this log a LOWER BOUND: it cannot distinguish two live instances from
# three-with-one-line-thrown-away, so the count is not evidence at that point.
# Unlike the deferral itself this clause is checked, not assumed; the teeth
# drive it with a canned log carrying the kernel's exact line.
#
# Prerequisite: the E3 ISO, which is NOT the shipped image — build it with
#   make x86-iso-e3
# (init compiled --features e3_envs, packed into sidecars_e3.cpio, embedded in
# sls_operating_system_e3.iso). Also needs qemu-system-x86_64, or a stand-in via
# E3_QEMU (the teeth use one). Reads nothing else; touches only its temp log.
#
# GUARD-KIND: runtime (needs the built E3 ISO + QEMU).
#
# Exit: 0 if both tenant environments came up at distinct storage addresses,
# 1 if QEMU died first or a marker/assertion failed, 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E3_ISO:-sls_operating_system_e3.iso}"
QEMU="${E3_QEMU:-qemu-system-x86_64}"   # overridable so the teeth can feed a
                                          # canned boot log without an ISO
LOG=/tmp/aerosls_e3_multi_env.log
WINDOW_S=120              # the tenant envs are spawned last (after the system
                          # POSIX's network boot), so give the whole chain room

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso-e3" >&2; exit 2; }
command -v "${QEMU%% *}" >/dev/null 2>&1 || { echo "ABORT: ${QEMU%% *} not installed" >&2; exit 2; }

rm -f "$LOG"
# Same device model as phase5_boot_smoke / x86-run: the system POSIX still does
# its network + DEV bring-up (the tenants use neither). The tenant environments
# ride on top of an otherwise-normal Phase 5 boot.
$QEMU -cdrom "$ISO" -display none -m 4G -no-reboot \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -serial file:"$LOG" 2>/dev/null &
QPID=$!

# ONE signal gates the assertions, polled (not a fixed sleep) so the check waits
# exactly as long as the tenants — which are scheduled LAST — need: three
# '[POSIX] aero state=00000000' lines, i.e. the system POSIX plus BOTH tenants
# have each brought up a live (formatted) rootfs.
#
# init's "tenant environments spawned" marker is REPORTED, not gating. It
# travels init's sidecar-console path, which is the lossy one — the kernel
# coalesces sidecar messages and demonstrably drops bytes (measured: that
# marker is present in one boot and absent in the next, with every other line
# of the same batch intact). Gating on it made the guard fail on a machine that
# had reported the very property it asserts, and "the log does not show X" is
# not evidence that X did not happen. The three live-rootfs lines are NOT
# console-derived: each is a SYS_SLS_SERIAL_WRITE (165) straight to the UART,
# per line, under the kernel TX lock, and kernel TX during a loopback window is
# no longer discarded (deferred and replayed at release — kernel/kernel_io.c),
# so this evidence is drop-proof up to the kernel's ring bound. Past that bound
# the log is a lower bound, not an absence, and this check says so by name
# instead of reporting a `2/3` boot verdict (the evidence-integrity clause
# below reads the kernel's overflow line before the count means anything).
#
# The marker's absence is still stated in the verdict, because silently
# tolerating it would hide a real init regression behind a shrug.
done_marker="tenant environments spawned"
live_re="\[POSIX\] aero state=00000000"
marker_seen=0
booted=0
qemu_alive=1
for _i in $(seq 1 $((WINDOW_S / 2))); do
    live_n=$(grep -acE "$live_re" "$LOG" 2>/dev/null); live_n=${live_n:-0}
    grep -aqF "$done_marker" "$LOG" 2>/dev/null && marker_seen=1
    if [ "$live_n" -ge 3 ]; then
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

# ─── Evidence integrity, read before the count is allowed to mean anything ──
# The kernel prints this once, at replay, carrying the byte count it could not
# hold. Its presence proves the log lost machine output, so every later count is
# a LOWER BOUND rather than a measurement of the boot.
overflow_line="$(grep -aE '\[serial\] loopback window overflow: 0x[0-9a-fA-F]+ byte\(s\) dropped' "$LOG" 2>/dev/null | head -1)"
evidence_lost=0
[ -n "$overflow_line" ] && evidence_lost=1

if [ "$booted" -eq 0 ]; then
    live_n=$(grep -acE "$live_re" "$LOG" 2>/dev/null); live_n=${live_n:-0}
    if [ "$evidence_lost" -eq 1 ]; then
        # Takes precedence over BOTH other shapes: a truncated log cannot be
        # read as a boot outcome, and reporting `2/3` here is the exact lie this
        # clause exists to stop (the machine may well have brought every
        # instance up; the kernel dropped its proof on the wire).
        echo "FAIL  serial evidence was lost during a loopback window — the live-rootfs" >&2
        echo "      count is a LOWER BOUND, not a boot outcome (the machine may still have" >&2
        echo "      brought every instance up; the kernel dropped its proof on the wire):" >&2
        echo "      $overflow_line" >&2
        echo "      live rootfs seen: $live_n/3 (>= 3 is the property, so this boot is unproven)" >&2
        [ "$qemu_alive" -eq 0 ] && \
            echo "      note: QEMU also exited before the gate opened — see the tail below" >&2
    elif [ "$qemu_alive" -eq 0 ]; then
        echo "FAIL  QEMU exited before both tenant environments came up (live rootfs: $live_n/3)" >&2
    else
        echo "FAIL  within ${WINDOW_S}s: 3 live rootfs never seen (live rootfs: $live_n/3)" >&2
    fi
    echo "      last 40 serial lines:" >&2
    tail -40 "$LOG" >&2
    exit 1
fi

fail=0

# Evidence first: if the kernel dropped bytes, none of the lines below is a
# complete account of the boot, so the check fails by name before crediting
# anything it happened to keep. This is the clause that keeps a `2/3` reading
# from being reported as a boot failure when it is a log failure.
if [ "$evidence_lost" -eq 1 ]; then
    echo "FAIL  serial evidence was lost during a loopback window: $overflow_line" >&2
    echo "      the log is a LOWER BOUND on what the machine reported — its losses land on" >&2
    echo "      whichever writer was mid-line, so the surviving lines are not a full account" >&2
    fail=1
fi

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
# Deliberately loose about the LABEL and strict about the FACT. Two shapes have
# to survive: init's console output can lose a byte (the observed damage lands on
# names, not addresses), AND it is coalesced — the kernel's console buffer
# concatenates sidecar messages, so `E3 env 1: …` routinely sits at the END of a
# long line that already carries another '0x' address (`[INIT] e1000 BAR0 @
# 0xfeb80000 … [INIT] E3 env 1: drv.ramdisk.1 storage @ 0x1230e000`). Taking "the
# first 0x on the line" silently reads the BAR instead of the region — measured,
# and it made two genuinely distinct regions compare equal. So: anchor at
# `E3 env N:` and take the first hex AFTER it, skipping nothing that could hide
# another address (`[^@]*` cannot cross the storage address's own `@`).
# A damaged ADDRESS digit still cannot pass silently -- see the alignment check
# below, which a mis-parsed hex digit breaks in 15 cases out of 16.
addr_of() {
    grep -aoE "E3 env $1: [^@]*@ 0x[0-9a-fA-F]+" "$LOG" \
        | head -1 | grep -aoE "0x[0-9a-fA-F]+" | head -1
}
s1="$(addr_of 1)"
s2="$(addr_of 2)"
# A region is page-aligned (SYS_SLS_ALLOC_REGION with align_frames=1), so a
# mis-parsed digit shows up here instead of quietly comparing two wrong numbers.
align_ok=1
for a in "$s1" "$s2"; do
    case "$a" in 0x*000) ;; *) align_ok=0 ;; esac
done
if [ -z "$s1" ] || [ -z "$s2" ]; then
    echo "FAIL  could not read both tenant storage addresses (env1='$s1' env2='$s2')" >&2
    grep -aE "\[INIT\] E3 env [0-9]+: .* storage @" "$LOG" >&2
    fail=1
elif [ "$align_ok" -eq 0 ]; then
    echo "FAIL  a tenant storage address is not page-aligned (env1='$s1' env2='$s2') — a damaged log line, not a boot failure" >&2
    fail=1
elif [ "$s1" = "$s2" ]; then
    echo "FAIL  the two tenant environments share storage $s1 — regions are NOT isolated" >&2
    fail=1
else
    echo "ok:   tenant storage regions are distinct (env1=$s1, env2=$s2)"
fi

# The live-rootfs count IS the gate above (all three POSIX instances reach a
# LIVE rootfs: for a tenant that proves mount_aerofs_or_format formatted the
# blank k_alloc_region region successfully — a failed format never reaches this
# diagnostic; the sidecar's klog prints the u32 state as 8-digit zero-padded
# uppercase hex, so Live (0) is exactly 'state=00000000', and matching a bare
# 'state=0' would also catch stale codes like 00000003). Restated here as an
# assertion only so a reader of the passing output sees the property by name;
# the count cannot fail at this point, and an arm that must fail on it does so
# at the gate (see tests/e3_multi_env_boot_check_smoke.sh arm 'two-live').
echo "ok:   $live_n live-rootfs lines (>= 3: system POSIX + both tenants; the system's"
echo "      watchdog respawn reports one more, which is why this can read 4)"

if [ "$evidence_lost" -eq 0 ]; then
    echo "ok:   no serial evidence was dropped (the loopback deferral ring did not overflow)"
fi

if [ "$fail" -ne 0 ]; then
    echo "FAIL  E3 multi-instance boot check failed" >&2
    echo "      init E3 lines seen:" >&2
    grep -aE "\[INIT\] E3" "$LOG" >&2
    exit 1
fi

# Reported, never gating -- see the gate's own comment above.
if [ "$marker_seen" -eq 1 ]; then
    echo "ok:   init reported every tenant spawn attempt (E3 done-marker seen)"
else
    echo "note: init's E3 done-marker was not seen in this log (sidecar-console loss) —"
    echo "      not gating: the three live-rootfs lines above are the property itself"
fi

echo "OK: two tenant POSIX environments booted with distinct private ramdisks (E3 multi-instance)"
exit 0
