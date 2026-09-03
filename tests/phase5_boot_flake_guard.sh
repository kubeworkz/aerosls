#!/usr/bin/env bash
# tests/phase5_boot_flake_guard.sh — boots the Phase 5 ISO six times under
# the flakiest configuration found on target (-accel tcg,thread=multi with
# 4 vCPUs) and fails the build if ANY boot lacks both 'ALL PHASES PASS' and
# the interactive shell prompt.
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
# Both markers are required, deliberately:
#   * 'ALL PHASES PASS'  — the full irqtest sequence ran: bind, timer edges
#     (vector 32), serial IRQ4 loopback (vector 36), stuck-driver window,
#     watchdog respawn + clean re-bind, and the vector unbind before exit.
#   * the shell prompt    — init.rc ran to completion (nettest, irqtest,
#     devtest, caps, System ready) and the interactive sh is up. A phase-3
#     FAIL, a devtest FAIL, or a NETBOOT FAILED aborts init.rc at its
#     gate, so the prompt is the end-to-end "the boot script finished"
#     marker.
#
# The prompt check looks for '$ ' PRECEDED BY A NEWLINE (or at log start),
# not any bare '$ ' substring, so mid-line '$ ' inside applet output cannot
# false-positive, and the prompt-as-last-line (no trailing newline) is
# still caught.
#
# Build-time only: requires sls_operating_system.iso (make x86-iso, which
# also embeds sidecars.cpio when present) and qemu-system-x86_64. Reads
# nothing else; touches only its temp logs. Wired into
# .github/workflows/ci.yml in the self-hosted job right after the
# phase5_boot_smoke.sh step, which already built the ISO.
#
# Exit: 0 if all six boots showed both markers, 1 if any boot failed
# (prints each failing boot's last serial lines), 2 on a missing
# prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO=sls_operating_system.iso
BOOTS="${BOOTS:-6}"          # the flake was ~1/6: six boots catch it with margin
WINDOW_S=120               # generous per-boot window (phase chain takes tens
                           # of seconds; boots that finish early are killed
                           # as soon as both markers appear)
FAILS=0

[ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed" >&2; exit 2; }

# has_prompt LOG — 0 if the log carries a real shell prompt ('$ ' at the
# start of a line, including the last unterminated line), 1 otherwise.
# A log that does not exist yet (QEMU has not created it) counts as
# no-prompt, silently — the poll loop re-checks next iteration.
has_prompt() {
    python3 - "$1" <<'PYEOF'
import os, sys
p = sys.argv[1]
if not os.path.exists(p):
    sys.exit(1)
d = open(p, "rb").read()
sys.exit(0 if (d.startswith(b"$ ") or b"\n$ " in d) else 1)
PYEOF
}

for b in $(seq 1 "$BOOTS"); do
    LOG="/tmp/aerosls_flake_guard_boot${b}.log"
    rm -f "$LOG"
    start=$(date +%s)
    qemu-system-x86_64 -cdrom "$ISO" -display none -m 4G -smp 4 \
        -accel tcg,thread=multi -boot d -no-reboot \
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
        -serial file:"$LOG" 2>/dev/null &
    QPID=$!

    phases_ok=0
    prompt_ok=0
    qemu_alive=1
    for i in $(seq 1 $((WINDOW_S / 2))); do
        if grep -aqF "ALL PHASES PASS" "$LOG" 2>/dev/null; then
            phases_ok=1
        fi
        if has_prompt "$LOG"; then
            prompt_ok=1
        fi
        if [ "$phases_ok" -eq 1 ] && [ "$prompt_ok" -eq 1 ]; then
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

    if [ "$phases_ok" -eq 1 ] && [ "$prompt_ok" -eq 1 ]; then
        echo "OK   boot ${b}/${BOOTS}: 'ALL PHASES PASS' + shell prompt (${elapsed}s)"
    else
        FAILS=$((FAILS + 1))
        echo "FAIL boot ${b}/${BOOTS}: markers after ${elapsed}s —" \
             "phases=$phases_ok prompt=$prompt_ok qemu_alive=$qemu_alive"
        if [ "$qemu_alive" -eq 0 ]; then
            echo "      QEMU exited before both markers appeared — the boot crashed" >&2
        else
            echo "      window (${WINDOW_S}s) ran out before both markers appeared" >&2
        fi
        echo "      last 20 serial lines:" >&2
        tail -20 "$LOG" >&2
    fi
done

if [ "$FAILS" -eq 0 ]; then
    echo "OK: ${BOOTS}/${BOOTS} boots clean under -accel tcg,thread=multi (no phase-3 / respawn flake)"
    exit 0
fi
echo "FAIL: ${FAILS}/${BOOTS} boots missed 'ALL PHASES PASS' and/or the shell prompt — the flake is back" >&2
exit 1