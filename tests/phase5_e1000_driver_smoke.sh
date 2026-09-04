#!/usr/bin/env bash
# tests/phase5_e1000_driver_smoke.sh — asserts the DM actually spawns the
# drv.e1000.0 driver sidecar on the real target: boots the ISO under QEMU
# with TWO e1000 NICs and requires '[e1000] PASS' (the driver's PHY-loopback
# round trip over the NIC the kernel handed off) to reach the serial log.
#
# ─── Why two NICs ─────────────────────────────────────────────────────────
# The kernel's own net stack drives the first e1000 (grub nic0=both: mgmt +
# cluster — the -netdev user card). A userspace driver on that card would
# fight the kernel for the same registers, so the kernel only hands over a
# NIC whose assigned role is NONE. The second -device e1000 (no netdev) is
# nic1=none: the kernel enables PCI bus mastering, marks the BAR
# uncacheable (e1000_driver_handoff), and never touches it again. The
# device registry marks THAT card drv.e1000.0 (role-aware — kernel-owned
# NICs stay driverless), and when init's registry message reaches the DM,
# the DM builds the driver manifest (image grant + registry BAR0 -> DEV
# cap, e1000.heap -> budget) and calls create_sidecar. The driver maps
# BAR0 via SYS_DEV_MMAP, programs TX/RX rings in its budget region, and
# proves a PHY-loopback frame round trip: '[e1000] PASS'.
#
# A regression anywhere in that chain — the loader's e1000.image copy, the
# init e1000.image cap + grant, the DM spawn decision, the kernel's
# role-aware registry marking, the create_sidecar DEV/MEM mints, the
# driver's SYS_DEV_MMAP path, or the loopback bring-up — shows up as a
# missing '[e1000] ...' line while the rest of the system still boots.
#
# Both NICs carry deterministic MACs (52:54:00:12:34:0X). QEMU assigns PCI
# slots in -device order, so the netdev card is nic0 and the bare card is
# nic1, matching grub's nic0=both nic1=none.
#
# Build-time only: requires sls_operating_system.iso (make x86-iso, which
# also embeds sidecars.cpio when present) and qemu-system-x86_64. Reads
# nothing else; touches only its temp log. Wired into .github/workflows/
# ci.yml in the self-hosted job right after the phase-5 boot smoke.
#
# Exit: 0 if 'e1000] PASS' appeared, 1 if QEMU died first or the window ran
# out, 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO=sls_operating_system.iso
LOG=/tmp/aerosls_phase5_e1000_smoke.log
WINDOW_S=180               # generous: the chain (init -> DM -> registry ->
                           # spawn -> driver bring-up + the ~1 s QEMU RX
                           # grace settle) takes tens of seconds, and under
                           # contended TCG the guest can crawl 3-5x slower
                           # than nominal — a PASS that lands late is still
                           # a PASS (thread=multi boots that stay healthy
                           # finish in ~15 s)

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }

# The QEMU RX grace (e1000's 1000 ms post-RCTL flush_queue_timer) must
# expire in WALL time for a retried TX to land, and QEMU only advances its
# virtual-clock timers against the real clock when the emulated main loop
# runs. Under the default single-threaded TCG a spinning guest starves
# that loop for tens of seconds to minutes, which made this smoke's
# 120 s window a coin flip. -accel tcg,thread=multi runs the timer
# iothread in real time (same config the phase-5 boot flake guard uses),
# so the grace reliably expires ~1 s after RX-enable and the driver's
# bounded retry ladder lands PASS within seconds.
rm -f "$LOG"
qemu-system-x86_64 -cdrom "$ISO" -display none -m 4G -no-reboot \
    -accel tcg,thread=multi \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -device e1000,mac=52:54:00:12:34:02 \
    -serial file:"$LOG" 2>/dev/null &
QPID=$!

e1000_ok=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S / 2))); do
    if grep -qF "e1000] PASS" "$LOG" 2>/dev/null; then
        e1000_ok=1
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

if [ "$e1000_ok" -eq 1 ]; then
    echo "OK: 'e1000] PASS' reached the serial log (DM spawned drv.e1000.0 on the handed-off NIC)"
    exit 0
fi
if [ "$qemu_alive" -eq 0 ]; then
    echo "FAIL  QEMU exited before 'e1000] PASS' appeared — the Phase 5 boot crashed" >&2
else
    echo "FAIL  'e1000] PASS' missing in the boot log within ${WINDOW_S}s" >&2
fi
echo "      last 30 serial lines:" >&2
tail -30 "$LOG" >&2
exit 1
