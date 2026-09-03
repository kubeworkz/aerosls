#!/usr/bin/env bash
# tests/phase5_boot_smoke.sh — asserts the Phase 5 self-hosted boot's
# driver-SDK demo actually runs on the real target: boots the ISO under
# QEMU and requires both 'irqtest PASS' and 'devtest PASS' to reach the
# serial log.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The irqtest applet (user/sidecar/src/applets.rs, run by init.rc) is the
# end-to-end proof of the Driver SDK IRQ and IO capabilities: it binds the
# manifest IRQ caps, parks on the notification channels, and counts
# hardware edges (the LAPIC timer at vector 32, plus the serial port's
# IRQ4 at vector 36 delivered through the IO-APIC). The devtest applet
# proves the third capability type — CAP_TYPE_DEV: it maps the e1000's
# MMIO BAR0 via SYS_DEV_MMAP (a DEV cap the init sidecar minted into the
# POSIX manifest from the device registry's PCI scan) and reads the
# device's MAC out of its RAL0/RAH0 registers. A regression anywhere in
# that chain — the manifest minting (cap_create_sidecar), the syscall
# dispatch, the IRQ registry, the stub wiring, the self-mask/re-arm
# dance, or the DEV object/map path — shows up as a missing or failing
# '[irqtest] ...' / '[devtest] ...' line in the serial log while the
# rest of the system still boots. This smoke makes CI see it: the
# self-hosted job already boots the ISO for /api/health; this script
# gives the driver-SDK demos their own bounded window and fails the build
# when either PASS line never arrives.
#
# The QEMU command carries the SAME e1000 the Makefile's x86-run uses
# (-netdev user + -device e1000, mac 52:54:00:12:34:01), so the device
# registry actually contains an e1000 and devtest exercises the DEV path
# rather than skipping (a boot without -device e1000 has no DEV cap and
# devtest prints SKIP).
#
# It deliberately greps the same shape a human would look for in the boot
# log (the patterns 'irqtest] PASS' and 'devtest] PASS') so the gate
# matches whatever phase of the demo is enabled by the manifest, without
# hard-coding phase-specific text here.
#
# Build-time only: requires sls_operating_system.iso (make x86-iso, which
# also embeds sidecars.cpio when present — commit it so CI ships it) and
# qemu-system-x86_64. Reads nothing else; touches only its temp log.
# Wired into .github/workflows/ci.yml in the self-hosted job, right after
# the /api/health boot step.
#
# Exit: 0 if 'irqtest PASS' AND 'devtest PASS' appeared, 1 if QEMU died
# first or the window ran out, 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO=sls_operating_system.iso
LOG=/tmp/aerosls_phase5_smoke.log
WINDOW_S=90               # generous: the Phase-5 chain (init -> DM -> ramdisk
                          # -> network -> POSIX -> init.rc: nettest -> irqtest
                          # -> devtest) takes tens of seconds booting single-core

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }

rm -f "$LOG"
qemu-system-x86_64 -cdrom "$ISO" -display none -m 4G -no-reboot \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -serial file:"$LOG" 2>/dev/null &
QPID=$!

irq_ok=0
dev_ok=0
qemu_alive=1
for i in $(seq 1 $((WINDOW_S / 2))); do
    if grep -qF "irqtest] PASS" "$LOG" 2>/dev/null; then
        irq_ok=1
    fi
    if grep -qF "devtest] PASS" "$LOG" 2>/dev/null; then
        dev_ok=1
    fi
    if [ "$irq_ok" -eq 1 ] && [ "$dev_ok" -eq 1 ]; then
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

if [ "$irq_ok" -eq 1 ] && [ "$dev_ok" -eq 1 ]; then
    echo "OK: 'irqtest] PASS' and 'devtest] PASS' reached the serial log (Driver SDK IRQ + DEV demos green on target)"
    exit 0
fi
if [ "$qemu_alive" -eq 0 ]; then
    echo "FAIL  QEMU exited before both PASS lines appeared — the Phase 5 boot crashed" >&2
else
    echo "FAIL  markers missing in the boot log within ${WINDOW_S}s:" >&2
    echo "        irqtest] PASS: $irq_ok   devtest] PASS: $dev_ok" >&2
fi
echo "      last 30 serial lines:" >&2
tail -30 "$LOG" >&2
exit 1