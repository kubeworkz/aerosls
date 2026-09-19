#!/usr/bin/env bash
# tests/phase5_boot_flake_guard.sh — boots the Phase 5 ISO six times under
# the flakiest configuration found on target (-accel tcg,thread=multi with
# 4 vCPUs) and fails the build if ANY boot did not finish.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The irqtest phase-3 serial-IRQ windows were edge-triggered and racy under
# TCG multi-threaded: a stale looped-back byte could leave the UART line
# HIGH so the next THR write produced no rising edge, and ~1/6 to ~1/2 of
# boots died at 'ch=recover' (or later: a respawned client's NET_INFO was
# never served, or a live armed vector raced the shell fork). A single boot
# (phase5_boot_smoke.sh) can pass 6 times in a row while the flake still
# ships. This guard boots repeatedly so a one-in-six regression fails CI.
#
# The boot carries the same e1000 the Makefile x86-run uses (-netdev user
# + -device e1000, mac 52:54:00:12:34:01), so the device registry holds an
# e1000 and the devtest applet's CAP_TYPE_DEV path (SYS_DEV_MMAP of the
# MMIO BAR0 + MAC read) is exercised on every boot alongside irqtest.
#
# ─── What "finished" means (and what it deliberately does not) ─────────────
# A boot counts as finished when its log carries both of:
#
#   * 'ALL PHASES PASS'  — the full irqtest sequence ran: bind, timer edges
#     (vector 32), serial IRQ4 loopback (vector 36), stuck-driver window,
#     watchdog respawn + clean re-bind, and the vector unbind before exit.
#   * the shell prompt   — the interactive sh is up, which can only happen
#     after init.rc ran to completion (nettest, irqtest, devtest, caps; any
#     phase FAIL, devtest FAIL or NETBOOT FAILED aborts init.rc at its gate).
#
# 'System ready' — the line init.rc's caps applet prints just before the
# shell — is measured and reported but NOT required, because the console
# mangles it: across 185 recorded logs it arrived one byte short
# ('ystem ready'), with a byte spliced in ('Sys\ntem ready'), or splintered
# ('$ $ stem ready'), while the phase marker was intact 162/162.
#
# The prompt test used to be "newline followed by '$ '". That rejected the
# shell's OWN prompt whenever its two prompt writes landed byte-interleaved
# — the shape every recorded failure had:
#
#     ... System ready\n$$      (glued — no '\n$ ')
#     ... System ready\n$ $     (two prompts, second partial)
#     ... System ready\n\n$     (clean)
#
# 130 surviving boot logs split 83 clean / 12 '$ $' / 10 '$$ '; the ten
# glued ones are the false failures this file used to report — phases OK,
# 'System ready' present, QEMU alive, log ending in the shell's own prompt
# bytes. One of those was caught live under a socket console and answered an
# injected command: the guest was running, only the log was torn. The prompt
# is text on a shared console, so it is judged as prompt *bytes* in any
# order (tests/phase5_boot_markers.py classifies the shapes, flags the glued
# ones as torn, and tolerates one damaged byte in each marker).
#
# Root cause, fixed 2026-09-18 (kernel/console_service.c): the mangling was
# not two writers racing on the wire -- the TX lock serialises those -- but
# the two DRAINERS. console_service_tick() runs on the AP's service poll and
# on the BSP's deferred timer tick, and both printed out of one static
# console_svc_buf; the second refilled it while the first was still shifting
# the message out, so a message went to the wire with another message's bytes
# spliced into it. The drain now carries the same compare-and-swap
# single-flight the RX forward beside it has had all along, and
# tests/console_tx_atomic_host_test.c holds that property on the host (a
# rival tick arriving inside the print loop must print nothing, and two
# queued messages must come out whole and in order). The one-damaged-byte
# tolerance below stays: it is defence in depth against any other lossy path,
# not the answer to this one.
#
# The guard therefore fails only on a boot that did not reach its end
# state, and it says *which* marker was missing when it does. To make a host
# episode distinguishable from a regression in that report, each boot's host
# pressure and reclaim counters are sampled and printed on failure (a
# nonzero pswpout/pgsteal means the runner was reclaiming under the guest).
#
# Build-time only: requires sls_operating_system.iso (make x86-iso, which
# also embeds sidecars.cpio when present) and qemu-system-x86_64. Reads
# nothing else; touches only its temp logs. Wired into
# .github/workflows/ci.yml in the self-hosted job right after the
# phase5_boot_smoke.sh step, which already built the ISO.
#
# Overridable for the teeth (tests/phase5_boot_flake_guard_smoke.sh drives
# this file with a stub qemu and a dummy ISO): ISO, BOOTS, WINDOW_S, QEMU.
#
# Exit: 0 if all six boots finished, 1 if any boot did not (prints each
# failing boot's markers and last serial lines), 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${ISO:-sls_operating_system.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
BOOTS="${BOOTS:-6}"          # the flake was ~1/6: six boots catch it with margin
WINDOW_S="${WINDOW_S:-120}"  # generous per-boot window (phase chain takes tens
                             # of seconds; boots that finish early are killed
                             # as soon as all three markers appear)
MARKERS=tests/phase5_boot_markers.py
FAILS=0
TORN=0

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v "$QEMU" >/dev/null 2>&1 || { echo "ABORT: $QEMU not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed" >&2; exit 2; }

# host_state — one line of host-side pressure/reclaim counters. Printed on
# failure so a runner that was thrashing under the guest is visible in CI
# instead of being inferred from a wedge. Missing files (non-Linux host)
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

# boot_once — one launch-and-poll cycle for the current $LOG. Sets the
# globals the report uses: healthy, qemu_alive, markers, elapsed, host_before.
boot_once() {
    rm -f "$LOG" "$QERR"
    start=$(date +%s)
    host_before=$(host_state)
    "$QEMU" -cdrom "$ISO" -display none -m 4G -smp 4 \
        -accel tcg,thread=multi -boot d -no-reboot \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
        -serial file:"$LOG" 2>"$QERR" &
    QPID=$!

    healthy=0
    qemu_alive=1
    markers=""
    for i in $(seq 1 $((WINDOW_S / 2))); do
        if markers=$(python3 "$MARKERS" --check "$LOG" 2>/dev/null); then
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
        if markers=$(python3 "$MARKERS" --check "$LOG" 2>/dev/null); then
            healthy=1
        fi
    fi
}

for b in $(seq 1 "$BOOTS"); do
    LOG="/tmp/aerosls_flake_guard_boot${b}.log"
    QERR="$LOG.qemuerr"
    boot_once
    retried=""
    # A QEMU that died before the guest printed anything *and* left a
    # diagnostic never ran this tree — that is the host (its stderr says so:
    # 'Cannot allocate', 'cannot set up guest memory', 'terminating on signal
    # N'), so it is retried once rather than counted. A guest reset (triple
    # fault) leaves QEMU's stderr EMPTY and is never retried: that is a
    # genuine failure of the tree, which is why the retry keys on the
    # diagnostic and not on 'it exited early'.
    if [ "$healthy" -eq 0 ] && [ "$qemu_alive" -eq 0 ] && [ -s "$QERR" ]; then
        echo "RETRY boot ${b}/${BOOTS}: QEMU left a diagnostic instead of a boot — $(tr '\n' ' ' < "$QERR" | cut -c1-200)" >&2
        boot_once
        retried=" after a retry"
    fi

    case "$markers" in
        *"torn=1"*) TORN=$((TORN + 1)) ;;
    esac

    if [ "$healthy" -eq 1 ]; then
        echo "OK   boot ${b}/${BOOTS}: 'ALL PHASES PASS' + shell prompt (${elapsed}s${retried})"
    else
        FAILS=$((FAILS + 1))
        echo "FAIL boot ${b}/${BOOTS}: boot did not finish after ${elapsed}s (qemu_alive=$qemu_alive)" >&2
        echo "      markers: ${markers:-none (no log written)}" >&2
        if [ "$qemu_alive" -eq 0 ]; then
            echo "      QEMU exited before the markers appeared" >&2
            # Calibrated elsewhere on this tree: a guest reset (triple fault)
            # leaves QEMU's stderr EMPTY, an externally killed QEMU prints
            # 'terminating on signal N', and a host resource failure names
            # itself ('cannot set up guest memory', 'Cannot allocate').
            # Printing which of the three happened is what separates a host
            # episode from a regression in the report.
            if [ -s "$QERR" ]; then
                echo "      qemu stderr: $(tr '\n' ' ' < "$QERR" | cut -c1-300)" >&2
            else
                echo "      qemu stderr empty — a guest reset leaves it empty (a killed QEMU would say 'terminating on signal N')" >&2
            fi
            if [ "$(wc -c < "$LOG" 2>/dev/null || echo 0)" -lt 4096 ]; then
                echo "      the log stopped before the guest printed anything — a launch failure or a reset at handoff, not a phase-chain stop" >&2
            fi
        else
            echo "      window (${WINDOW_S}s) ran out before the markers appeared" >&2
        fi
        echo "      host before: ${host_before:-unavailable}" >&2
        echo "      host after:  $(host_state)" >&2
        echo "      last 20 serial lines:" >&2
        tail -20 "$LOG" >&2
    fi
done

if [ "$FAILS" -eq 0 ]; then
    echo "OK: ${BOOTS}/${BOOTS} boots finished under -accel tcg,thread=multi (no phase-3 / respawn flake)"
    if [ "$TORN" -gt 0 ]; then
        echo "NOTE: ${TORN}/${BOOTS} boots logged the prompt byte-interleaved ('\$\$ ') — the console's" >&2
        echo "      two prompt writers sharing one UART. Cosmetic and counted, never a failure;" >&2
        echo "      see tests/phase5_boot_markers.py for the three shapes." >&2
    fi
    exit 0
fi
echo "FAIL: ${FAILS}/${BOOTS} boots did not finish — the flake is back" >&2
exit 1
