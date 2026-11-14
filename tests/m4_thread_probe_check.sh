#!/usr/bin/env bash
# tests/m4_thread_probe_check.sh — live QEMU gate for M4.2's clone path.
#
# GUARD-KIND: runtime (needs the built M4 ISO + QEMU)
#
# The probe (user/sidecar/fixtures/m4_thread_probe.S) is a Linux task spawned
# by the initial POSIX sidecar. It issues raw clone(56); the child writes
#      M4-THREAD-OK
# through its own tracked messenger, publishes a shared stack word, then
# faults; the parent observes the word and writes
#      M4-THREAD-PARENT-OK
# before exit_group. The child marker appearing BEFORE the parent marker is
# the live proof that two execution contexts shared one address space, that
# the child resumed at the parent's post-syscall RIP with rax=0, and that its
# write reached the sidecar on its own channel.
#
# This gate fails on any `M4-THREAD-FAIL`, a missing child or parent marker,
# the parent marker appearing before the child marker, or no marker inside
# the observation window.
#
# Environment:
#   M4_ISO          image to boot       (default: sls_m4_thread.iso)
#   QEMU            emulator command    (default: qemu-system-x86_64)
#   M4_GATE_WINDOW_S seconds to observe  (default: 120)
#   M4_GATE_KEEP    keep the serial log on success (default: 0)
#   M4_GATE_LOG     serial log path     (default: /tmp/m4_thread_probe.$$.log)
#   M4_GATE_REPLAY  validate an existing log instead of booting QEMU
#
# Build:
#   make x86-iso M4_THREAD_PROBE=1 X86_ISO=sls_m4_thread.iso
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${M4_ISO:-sls_m4_thread.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
WINDOW_S="${M4_GATE_WINDOW_S:-120}"
REPLAY="${M4_GATE_REPLAY:-}"
KEEP="${M4_GATE_KEEP:-0}"
LOG="${M4_GATE_LOG:-/tmp/m4_thread_probe.$$.log}"
QERR="${LOG}.qemuerr"
QPID=""
STARTED="0"

case "$WINDOW_S" in
    ''|*[!0-9]*|0)
        echo "ABORT: M4_GATE_WINDOW_S must be a positive integer (got '$WINDOW_S')" >&2
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
        echo "ABORT: M4_GATE_REPLAY is not a file: $REPLAY" >&2
        exit 2
    }
    LOG="$REPLAY"
    QERR=""
else
    [ -f "$ISO" ] || {
        echo "ABORT: missing M4 ISO '$ISO' (build with: make x86-iso M4_THREAD_PROBE=1 X86_ISO=$ISO)" >&2
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
        fail=$(count_fixed 'M4-THREAD-FAIL')
        child=$(count_fixed 'M4-THREAD-OK')
        parent=$(count_fixed 'M4-THREAD-PARENT-OK')
        if [ "$fail" -gt 0 ] || { [ "$child" -gt 0 ] && [ "$parent" -gt 0 ]; }; then
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

fail=$(count_fixed 'M4-THREAD-FAIL')
child=$(count_fixed 'M4-THREAD-OK')
parent=$(count_fixed 'M4-THREAD-PARENT-OK')
clone_line=$(count_fixed '[PROC] M4.2 clone:')

problem=""
if [ "$fail" -gt 0 ]; then
    problem="serial log contains $fail M4-THREAD-FAIL marker(s)"
elif [ "$child" -lt 1 ]; then
    problem="no M4-THREAD-OK marker (the clone child never wrote through its own channel)"
elif [ "$parent" -lt 1 ]; then
    problem="no M4-THREAD-PARENT-OK marker (the parent never observed the shared word)"
elif ! awk '
    /M4-THREAD-OK/ { child = NR }
    /M4-THREAD-PARENT-OK/ && child && !seen { seen = 1 }
    END { exit seen ? 0 : 1 }
' "$LOG"; then
    problem="M4-THREAD-PARENT-OK appeared before any M4-THREAD-OK (shared word was not ordered)"
fi

if [ -n "$problem" ]; then
    echo "FAIL: M4.2 live clone probe: $problem" >&2
    echo "  child=$child parent=$parent fail=$fail clone_line=$clone_line" >&2
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

echo "OK: M4.2 live clone probe: child=$child parent=$parent fail=0 clone_lines=$clone_line"
if [ "$KEEP" = "1" ] || [ -n "$REPLAY" ]; then
    echo "    serial log: $LOG"
fi
exit 0
