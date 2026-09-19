#!/usr/bin/env bash
# tests/phase5_boot_smoke.sh — asserts the Phase 5 self-hosted boot's
# driver-SDK demos actually run on the real target: boots the ISO under QEMU
# and requires both the irqtest demo ('[irqtest] PASS') and the DEV demo
# ('[devtest] PASS') to reach the serial log.
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
# It requires the two demo PASS lines rather than the end state the flake
# guard requires ('ALL PHASES PASS' + shell prompt) on purpose: this is the
# gate that says the IRQ/DEV capability path ran, and it is deliberately the
# earlier, narrower signal. The flake guard behind it boots six times and
# judges the end state.
#
# The QEMU command carries the SAME e1000 the Makefile's x86-run uses
# (-netdev user + -device e1000, mac 52:54:00:12:34:01), so the device
# registry actually contains an e1000 and devtest exercises the DEV path
# rather than skipping (a boot without -device e1000 has no DEV cap and
# devtest prints SKIP).
#
# ─── How the markers are judged (and why not with a literal grep) ──────────
# tests/phase5_boot_markers.py classifies the log — the same classifier the
# flake guard uses, driven with `--preset demos`. A literal `grep -F` was the
# old judge, and it had the same false-negative class the guard was fixed
# for: the serial log is measurably not byte-faithful (one byte dropped,
# spliced, or substituted is the common shape — see the classifier's header
# for the counts), so a healthy '[devtest] PASS' that arrives one byte short
# read as "the demo never ran". Each marker is therefore matched intact or
# with a single damaged byte, damage is reported ('dev=1*') instead of
# silently absorbed, and it never manufactures a marker that is absent.
#
# The report is per-marker and names the stage: a boot that stopped after
# the serial-IRQ unmask prints
#
#     need=irq,dev … irq=1 dev=0 … stage=[IRQ] unmask vector 36: pin 4 …
#
# so a failure says where it stopped rather than only that it failed.
#
# ─── The window is sized from measurement, not from a guess ───────────────
# This smoke boots single-core (no -smp, so one vCPU and single-threaded
# TCG) while the flake guard boots four. Timed on the reference host with a
# 0.5s poll, one healthy boot wrote its markers at:
#
#     first '[irqtest] PASS'              62.0s
#     '[irqtest] ALL PHASES PASS' + '[devtest] PASS'  100.6s
#     'System ready' + shell prompt       100.7s
#
# The window used to be 90s — below the 100.6s a HEALTHY boot needs, and on
# a loaded runner further below still. That made the smoke fail healthy boots
# on timing alone, which is the same class of false failure the literal grep
# produced, so the default is now 150s (about 1.5x the measured healthy time)
# and the whole boot is still killed the moment both PASS lines arrive. A
# boot that really stalled still fails: the window exists to stop waiting, not
# to pass anything, and the report says which marker was missing and how far
# the boot got.
#
# ─── Why a failure report distinguishes three different failures ───────────
# The single boot this smoke takes is the smoke most likely to draw a host
# episode: it runs on the shared self-hosted runner, and a QEMU that dies of
# host memory pressure is not a regression in this tree. So QEMU's stderr is
# captured (the old script sent it to /dev/null) and the failure is
# classified:
#
#   * QEMU alive, markers missing      — the guest stalled or ran out of
#                                        window; the stage line names where,
#                                        and the report says the guest was
#                                        still alive (it did not crash).
#   * QEMU dead, stderr EMPTY          — a guest reset (triple fault) leaves
#                                        it empty: a real failure of the tree,
#                                        never retried.
#   * QEMU dead, stderr non-empty      — the host or an external kill; the
#                                        script names it and retries ONCE.
#
# Only the last of those is retried, and only when the process wrote a
# diagnostic: a retry keyed on "it exited early" would mask a reset.
#
# Build-time only: requires sls_operating_system.iso (make x86-iso, which
# also embeds sidecars.cpio when present — commit it so CI ships it) and
# qemu-system-x86_64. Reads nothing else; touches only its temp log.
# Wired into .github/workflows/ci.yml in the self-hosted job, right after
# the /api/health boot step.
#
# Overridable for the teeth (tests/phase5_boot_markers_smoke.sh drives this
# file with a stub qemu and a dummy ISO): ISO, QEMU, LOG, WINDOW_S.
#
# Exit: 0 if both PASS lines appeared, 1 if QEMU died first or the window
# ran out, 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${ISO:-sls_operating_system.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
LOG="${LOG:-/tmp/aerosls_phase5_smoke.log}"
WINDOW_S="${WINDOW_S:-150}"   # generous: the Phase-5 chain (init -> DM ->
                              # ramdisk -> network -> POSIX -> init.rc:
                              # nettest -> irqtest -> devtest) takes tens of
                              # seconds booting single-core, and this smoke
                              # shares a busy runner. A boot that really
                              # stalled still fails — the window exists to
                              # stop waiting, not to pass anything.
MARKERS=tests/phase5_boot_markers.py
QERR="$LOG.qemuerr"

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "ABORT: $QEMU not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed" >&2; exit 2; }
[ -f "$MARKERS" ] || { echo "ABORT: $MARKERS missing" >&2; exit 2; }

# host_state — one line of host-side pressure/reclaim counters. Printed on
# failure so a runner that was thrashing under the guest is visible in CI
# instead of being inferred from a stall. Missing files (non-Linux host)
# simply contribute nothing.
host_state() {
    {
        if [ -r /proc/pressure/memory ]; then
            awk '/^(some|full)/ {printf "psi_mem_%s=%s ", $1, $2}' /proc/pressure/memory
        fi
        if [ -r /proc/vmstat ]; then
            awk '/^(pgsteal_kswapd|pgsteal_direct|pswpout|pgmajfault|allocstall_movable) / \
                 {printf "%s=%s ", $1, $2}' /proc/vmstat
        fi
    } 2>/dev/null
}

# boot_once — one launch-and-poll cycle for $LOG. Sets the globals the report
# uses: healthy, qemu_alive, markers, elapsed, host_before.
boot_once() {
    rm -f "$LOG" "$QERR"
    start=$(date +%s)
    host_before=$(host_state)
    "$QEMU" -cdrom "$ISO" -display none -m 4G -no-reboot \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
        -serial file:"$LOG" 2>"$QERR" &
    QPID=$!

    healthy=0
    qemu_alive=1
    markers=""
    for i in $(seq 1 $((WINDOW_S / 2))); do
        if markers=$(python3 "$MARKERS" --check --preset demos "$LOG" 2>/dev/null); then
            healthy=1
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
    elapsed=$(( $(date +%s) - start ))
    # Re-read once more: a boot can finish inside the last poll interval.
    if [ "$healthy" -eq 0 ]; then
        if markers=$(python3 "$MARKERS" --check --preset demos "$LOG" 2>/dev/null); then
            healthy=1
        fi
    fi
}

boot_once
retried=""

# A QEMU that died before the demos ran *and* left a diagnostic never ran this
# tree — that is the host (its stderr says so: 'Cannot allocate', 'cannot set
# up guest memory', 'terminating on signal N'), so it is retried once rather
# than counted. A guest reset (triple fault) leaves QEMU's stderr EMPTY and is
# never retried: that is a genuine failure of the tree, which is why the retry
# keys on the diagnostic and not on 'it exited early'.
if [ "$healthy" -eq 0 ] && [ "$qemu_alive" -eq 0 ] && [ -s "$QERR" ]; then
    echo "RETRY: QEMU left a diagnostic instead of a boot — $(tr '\n' ' ' < "$QERR" | cut -c1-200)" >&2
    boot_once
    retried=" after a retry"
fi

if [ "$healthy" -eq 1 ]; then
    echo "OK: '[irqtest] PASS' and '[devtest] PASS' reached the serial log (Driver SDK IRQ + DEV demos green on target, ${elapsed}s${retried})"
    case "$markers" in
        *"irq=1*"*|*"dev=1*"*)
            echo "NOTE: a required marker matched with one damaged byte — the console mangled it, and the" >&2
            echo "      classifier tolerates it by design; markers: $markers" >&2
            ;;
    esac
    exit 0
fi

echo "FAIL  the driver-SDK demos did not both pass within ${WINDOW_S}s:" >&2
echo "      markers: ${markers:-none (no log written)}" >&2
if [ "$qemu_alive" -eq 0 ]; then
    # Calibrated elsewhere on this tree: a guest reset (triple fault) leaves
    # QEMU's stderr EMPTY, an externally killed QEMU prints 'terminating on
    # signal N', and a host resource failure names itself ('cannot set up
    # guest memory', 'Cannot allocate'). Printing which of the three happened
    # is what separates a host episode from a regression in the report.
    echo "      QEMU exited before both PASS lines appeared" >&2
    if [ -s "$QERR" ]; then
        echo "      qemu stderr: $(tr '\n' ' ' < "$QERR" | cut -c1-300)" >&2
    else
        echo "      qemu stderr empty — a guest reset leaves it empty (a killed QEMU would say 'terminating on signal N')" >&2
    fi
    if [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -lt 4096 ]; then
        echo "      the log stopped before the guest printed anything — a launch failure or a reset at handoff, not a demo that ran and failed" >&2
    fi
else
    echo "      the guest was alive when the window ran out — the boot stalled, it did not crash" >&2
fi
echo "      host before: ${host_before:-unavailable}" >&2
echo "      host after:  $(host_state)" >&2
echo "      last 30 serial lines:" >&2
tail -30 "$LOG" >&2
exit 1
