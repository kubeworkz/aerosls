#!/usr/bin/env bash
# tests/grub_select_kernel_only.sh — make a QEMU -cdrom boot select the
# grub "AeroSLS — kernel only" entry (menu entry 1) instead of the default
# Phase 5 self-hosted entry, by sending Down + Enter into the guest's
# SERIAL console before grub's 3 s countdown auto-boots entry 0.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# grub.cfg's entry 0 (the default every -cdrom boot lands on) loads the
# sidecars.cpio initrd. On that boot the kernel hands the machine to the
# init sidecar — launch_init_sidecar() enters it via kernel_enter_sidecar
# and never returns — and the comment in kernel/boot_image.c says it
# plainly: "The pre-Phase-5 shell/HTTP boot is skipped when an initrd is
# present — the sidecar system IS the boot." The kernel HTTP server
# (http_server_run, [HTTP] Listening on port 3000) therefore NEVER starts
# on a phase-5 boot.
#
# The HTTP-era guards (aerocap_boot_check, partition_teardown_boot_check,
# simi_teardown_boot_check, tcache_roundtrip_check) and CI's
# /api/health service smoke assert the classic kernel HTTP world: they
# boot the ISO and wait for the listener line or poll the forwarded
# /api/health endpoint. On the phase-5 default entry those assertions can
# never be satisfied, so every one of them failed CI even when the boot
# was perfectly healthy. The same ISO carries the classic boot as grub
# entry 1 ("kernel only" — no initrd module), which runs the legacy
# kernel.c path to http_server_run; this script selects that entry.
#
# ─── Why serial keys, not the QEMU monitor ────────────────────────────────
# QEMU's HMP `sendkey` injects PS/2 scancodes, but grub.cfg sets
# `terminal_input serial console` — grub reads its keyboard from the
# SERIAL port, so PS/2 keys never reach the menu (verified: sendkey leaves
# the countdown running and entry 0 boots). The boot's serial backend is
# therefore the input channel: grub's serial terminal maps ESC [ B to
# Down and CR to Enter. This helper writes exactly those bytes into the
# serial input fifo once the menu has rendered (its presence in the log
# means grub's serial terminal is live), so the keys abort the countdown,
# highlight entry 1, and boot it.
#
# Callers must run QEMU with a bidirectional serial backend whose host
# input fifo is `<SER>.in` and whose guest output is being captured to a
# log file (the standard `-serial pipe:$SER` + `cat $SER.out > log`
# pattern; `-serial file:` is write-only and cannot feed grub input).
#
# Usage (from a guard, right after QEMU is started):
#   bash tests/grub_select_kernel_only.sh "$SER.in" boot_part.log "$QPID" [entry]
#     $1 = the serial INPUT fifo (host writes guest-bound bytes here)
#     $2 = the serial-capture log (grub's menu render is detected here)
#     $3 = the QEMU pid (0 to skip the liveness check)
#     $4 = which menu entry to boot, 1-based MENU POSITION in grub.cfg,
#          default 2 — the "kernel only" entry, which is what every pre-E1
#          caller wants. The helper sends (entry - 1) Down keys from grub's
#          default highlight, so the default sends exactly the one Down its
#          name has always sent and menu entry 1 (the Phase-5 initrd boot)
#          sends none. grub.cfg's order is: 1 Phase 5, 2 kernel only,
#          3 unified (POSIX-Environments E1 — what
#          tests/unified_boot_check.sh selects), 4 GRUB shell.
#
# Exit: 0 once the Down(s)+Enter were written; 1 if QEMU died before grub's
# menu appeared or the menu never rendered within the wait — callers should
# treat 1 as a boot failure (their own HTTP/listener wait would fail
# anyway; failing here just names the cause earlier).
set -u

SER_IN="$1"
LOG="$2"
QPID="${3:-0}"
ENTRY="${4:-2}"
case "$ENTRY" in
    ''|*[!0-9]*|0)
        echo "FAIL: entry must be a 1-based menu position (got '$ENTRY')" >&2
        exit 1 ;;
esac

MENU_MARKER="Use the ^ and v keys"
WAIT_S=60   # grub renders its menu ~1-2 s after QEMU start even under slow
            # TCG; 60 s is a generous bound for a contended host.

# Only match a menu render that lands AFTER this helper started polling:
# a caller that reboots the guest (tcache_roundtrip_check's warm boot)
# calls this helper once per grub appearance, and a stale match against
# the previous boot's menu would inject the keys at the wrong moment.
SZ=0
if [ -f "$LOG" ]; then
    SZ=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
fi

saw_menu=0
for i in $(seq 1 $((WAIT_S * 5))); do   # poll every 0.2 s
    if [ -f "$LOG" ] && tail -c +$((SZ + 1)) "$LOG" 2>/dev/null | grep -aqF "$MENU_MARKER"; then
        saw_menu=1
        break
    fi
    if [ "$QPID" -ne 0 ] && ! kill -0 "$QPID" 2>/dev/null; then
        echo "FAIL: QEMU exited before the grub menu appeared" >&2
        exit 1
    fi
    sleep 0.2
done

if [ "$saw_menu" -eq 0 ]; then
    echo "FAIL: grub menu not seen in $LOG within ${WAIT_S}s — cannot select the 'kernel only' boot entry" >&2
    exit 1
fi

# ESC [ B = Down arrow (grub aborts its countdown and highlights the next
# entry); a short pause, then CR = Enter boots the highlighted entry. Written
# as two separate opens so a partial write can never interleave with another
# writer; grub holds the countdown open once the first Down lands.
DOWNS=""
for _i in $(seq 2 "$ENTRY"); do
    DOWNS="$DOWNS$(printf '\033[B')"
done
if ! printf '%s' "$DOWNS" > "$SER_IN" 2>/dev/null; then
    echo "FAIL: could not write the Down key(s) to $SER_IN" >&2
    exit 1
fi
sleep 0.5
if ! printf '\r' > "$SER_IN" 2>/dev/null; then
    echo "FAIL: could not write Enter to $SER_IN" >&2
    exit 1
fi
exit 0
