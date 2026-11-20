#!/usr/bin/env bash
# tests/m4_futex_probe_check.sh — live QEMU gate for M4.4's futex queue.
#
# GUARD-KIND: runtime (needs the built M4 futex ISO + QEMU)
#
# The fixture (user/sidecar/fixtures/m4_futex_probe.S) is a Linux task
# spawned by the initial POSIX sidecar. It clones a thread; the child
# publishes `ready` with FUTEX_WAKE, waits on `go`; the parent waits for
# `ready`, sets `go`, wakes the child, and waits for `done`. The child
# writes
#      M4-PTHREAD-OK
# and the parent then proves a timed wait returns -ETIMEDOUT before writing
#      M4-FUTEX-OK
#      M4-FUTEX-TIMEOUT-OK
# The child marker before the parent marker is the join-shaped proof: only a
# group-keyed wait queue can release the child, and only a timed wait can
# produce the third marker.
#
# Environment:
#   M4_FUTEX_ISO          image to boot       (default: sls_m4_futex.iso)
#   QEMU                  emulator command    (default: qemu-system-x86_64)
#   M4_FUTEX_WINDOW_S     seconds to observe  (default: 120)
#   M4_FUTEX_KEEP         keep the serial log on success (default: 0)
#   M4_FUTEX_LOG          serial log path     (default: /tmp/m4_futex_probe.$$.log)
#   M4_FUTEX_REPLAY       validate an existing log instead of booting QEMU
#
# Build (the probe is a sidecar cargo feature, so pack its archive first):
#   make X86_CC=gcc X86_LD=ld selfhost-bootimage M4_FUTEX_PROBE=1 \
#        SIDECAR_CPIO=sidecars_m4_futex.cpio
#   make X86_CC=gcc X86_LD=ld x86-iso SIDECAR_CPIO=sidecars_m4_futex.cpio \
#        X86_ISO=sls_m4_futex.iso
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${M4_FUTEX_ISO:-sls_m4_futex.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
WINDOW_S="${M4_FUTEX_WINDOW_S:-120}"
REPLAY="${M4_FUTEX_REPLAY:-}"
KEEP="${M4_FUTEX_KEEP:-0}"
LOG="${M4_FUTEX_LOG:-/tmp/m4_futex_probe.$$.log}"
QERR="${LOG}.qemuerr"
QPID=""
STARTED="0"

case "$WINDOW_S" in
    ''|*[!0-9]*|0)
        echo "ABORT: M4_FUTEX_WINDOW_S must be a positive integer (got '$WINDOW_S')" >&2
        exit 2
        ;;
esac

cleanup() {
    local rc=$?
    if [ -n "$QPID" ]; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    if [ "$KEEP" != "1" ] && [ -z "$REPLAY" ] && [ "$STARTED" = "1" ]; then
        rm -f "$LOG" "$QERR"
    fi
    return "$rc"
}
trap cleanup EXIT

count_fixed() {
    local n
    n=$(grep -aFc -- "$1" "$LOG" 2>/dev/null || true)
    printf '%s\n' "${n:-0}"
}

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || {
        echo "ABORT: M4_FUTEX_REPLAY is not a file: $REPLAY" >&2
        exit 2
    }
    LOG="$REPLAY"
    QERR=""
else
    [ -f "$ISO" ] || {
        echo "ABORT: missing M4 futex ISO '$ISO' (build with: make x86-iso M4_FUTEX_PROBE=1 X86_ISO=$ISO)" >&2
        exit 2
    }
    command -v "$QEMU" >/dev/null 2>&1 || {
        echo "ABORT: missing QEMU executable '$QEMU'" >&2
        exit 2
    }

    rm -f "$LOG" "$QERR"
    STARTED="1"
    "$QEMU" \
        -cdrom "$ISO" \
        -m 4G -smp 4 -accel tcg \
        -display none -monitor none -no-reboot -boot d \
        -serial "file:$LOG" \
        >/dev/null 2>"$QERR" &
    QPID=$!

    deadline=$(( $(date +%s) + WINDOW_S ))
    while kill -0 "$QPID" 2>/dev/null; do
        fail=$(count_fixed 'M4-FUTEX-FAIL')
        child=$(count_fixed 'M4-PTHREAD-OK')
        parent=$(count_fixed 'M4-FUTEX-OK')
        timed=$(count_fixed 'M4-FUTEX-TIMEOUT-OK')
        if [ "$fail" -gt 0 ] ||
           { [ "$child" -gt 0 ] && [ "$parent" -gt 0 ] && [ "$timed" -gt 0 ]; }; then
            sleep 1
            break
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            break
        fi
        sleep 2
    done
    if kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    QPID=""
fi

fail=$(count_fixed 'M4-FUTEX-FAIL')
child=$(count_fixed 'M4-PTHREAD-OK')
parent=$(count_fixed 'M4-FUTEX-OK')
timed=$(count_fixed 'M4-FUTEX-TIMEOUT-OK')
clone_line=$(count_fixed '[PROC] M4.2 clone:')
group_line=$(count_fixed 'clone group=')
wait_line=$(count_fixed 'wait (futex)')
wake_line=$(count_fixed 'wake (futex)')

problem=""
if [ "$fail" -gt 0 ]; then
    problem="serial log contains $fail M4-FUTEX-FAIL marker(s)"
elif [ "$child" -lt 1 ]; then
    problem="no M4-PTHREAD-OK marker (the futex wait child never returned)"
elif [ "$parent" -lt 1 ]; then
    problem="no M4-FUTEX-OK marker (the parent's join-shaped wake did not complete)"
elif [ "$timed" -lt 1 ]; then
    problem="no M4-FUTEX-TIMEOUT-OK marker (the timed wait arm did not return -ETIMEDOUT)"
elif [ "$group_line" -lt 1 ] || [ "$clone_line" -lt 1 ]; then
    problem="no kernel clone/sidecar group line (the child was not tracked in a group)"
elif [ "$wait_line" -lt 1 ] || [ "$wake_line" -lt 1 ]; then
    problem="no futex wait/wake line (the queue was not exercised)"
elif ! awk '
    /M4-PTHREAD-OK/ { child = NR }
    /M4-FUTEX-OK/ && child && !seen { seen = 1 }
    END { exit seen ? 0 : 1 }
' "$LOG"; then
    problem="M4-FUTEX-OK appeared before any M4-PTHREAD-OK (the join marker was not ordered)"
fi

if [ -n "$problem" ]; then
    echo "FAIL: M4.4 live futex probe: $problem" >&2
    echo "  child=$child parent=$parent timed=$timed fail=$fail clone_line=$clone_line group_line=$group_line wait_line=$wait_line wake_line=$wake_line" >&2
    if [ -f "$LOG" ]; then
        echo "  last 50 serial lines ($LOG):" >&2
        tail -50 "$LOG" >&2
    else
        echo "  serial log was not created: $LOG" >&2
    fi
    if [ -n "$QERR" ] && [ -s "$QERR" ]; then
        echo "  qemu stderr:" >&2
        tail -20 "$QERR" >&2
    fi
    exit 1
fi

echo "OK: M4.4 live futex probe: child=$child parent=$parent timed=$timed fail=0 clone_lines=$clone_line group_lines=$group_line wait_lines=$wait_line wake_lines=$wake_line"
if [ "$KEEP" = "1" ] || [ -n "$REPLAY" ]; then
    echo "    serial log: $LOG"
fi
exit 0
