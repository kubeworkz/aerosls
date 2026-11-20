#!/usr/bin/env bash
# tests/m4_group_probe_check.sh — live QEMU gate for M4.3's group state.
#
# GUARD-KIND: runtime (needs the built M4 group ISO + QEMU)
#
# The probe (user/sidecar/fixtures/m4_group_probe.S) is a Linux task spawned
# by the initial POSIX sidecar. The leader opens /tmp/m4-group, then raw
# clones with CLONE_FILES. The child writes that descriptor without opening
# it, writes
#      M4-GROUP-CHILD-OK
# and publishes a shared stack word; the parent observes the word and writes
#      M4-GROUP-PARENT-OK
# before exit_group. The child marker plus the parent marker is the live
# proof that the clone joined the leader's sidecar group: only a shared Linux
# fd table (and the VFS CLONE_FILES table) can make fd 3 writable in the
# child.
#
# This gate fails on any `M4-GROUP-FAIL`, a missing child or parent marker,
# the parent marker appearing before the child marker, or no marker inside
# the observation window.
#
# Environment:
#   M4_GROUP_ISO          image to boot       (default: sls_m4_group.iso)
#   QEMU                  emulator command    (default: qemu-system-x86_64)
#   M4_GROUP_WINDOW_S     seconds to observe  (default: 120)
#   M4_GROUP_KEEP         keep the serial log on success (default: 0)
#   M4_GROUP_LOG          serial log path     (default: /tmp/m4_group_probe.$$.log)
#   M4_GROUP_REPLAY       validate an existing log instead of booting QEMU
#
# Build (the probe is a sidecar cargo feature, so pack its archive first):
#   make X86_CC=gcc X86_LD=ld selfhost-bootimage M4_GROUP_PROBE=1 \
#        SIDECAR_CPIO=sidecars_m4_group.cpio
#   make X86_CC=gcc X86_LD=ld x86-iso SIDECAR_CPIO=sidecars_m4_group.cpio \
#        X86_ISO=sls_m4_group.iso
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${M4_GROUP_ISO:-sls_m4_group.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
WINDOW_S="${M4_GROUP_WINDOW_S:-120}"
REPLAY="${M4_GROUP_REPLAY:-}"
KEEP="${M4_GROUP_KEEP:-0}"
LOG="${M4_GROUP_LOG:-/tmp/m4_group_probe.$$.log}"
QERR="${LOG}.qemuerr"
QPID=""
STARTED="0"

case "$WINDOW_S" in
    ''|*[!0-9]*|0)
        echo "ABORT: M4_GROUP_WINDOW_S must be a positive integer (got '$WINDOW_S')" >&2
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
        echo "ABORT: M4_GROUP_REPLAY is not a file: $REPLAY" >&2
        exit 2
    }
    LOG="$REPLAY"
    QERR=""
else
    [ -f "$ISO" ] || {
        echo "ABORT: missing M4 group ISO '$ISO' (build with: make x86-iso M4_GROUP_PROBE=1 X86_ISO=$ISO)" >&2
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
        fail=$(count_fixed 'M4-GROUP-FAIL')
        child=$(count_fixed 'M4-GROUP-CHILD-OK')
        parent=$(count_fixed 'M4-GROUP-PARENT-OK')
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

fail=$(count_fixed 'M4-GROUP-FAIL')
child=$(count_fixed 'M4-GROUP-CHILD-OK')
parent=$(count_fixed 'M4-GROUP-PARENT-OK')
clone_line=$(count_fixed '[PROC] M4.2 clone:')
group_line=$(count_fixed 'clone group=')

problem=""
if [ "$fail" -gt 0 ]; then
    problem="serial log contains $fail M4-GROUP-FAIL marker(s)"
elif [ "$child" -lt 1 ]; then
    problem="no M4-GROUP-CHILD-OK marker (the clone child never wrote the leader's fd)"
elif [ "$parent" -lt 1 ]; then
    problem="no M4-GROUP-PARENT-OK marker (the parent never observed the shared group word)"
elif [ "$group_line" -lt 1 ]; then
    problem="no sidecar 'clone group=' line (the child was not tracked in a group)"
elif ! awk '
    /M4-GROUP-CHILD-OK/ { child = NR }
    /M4-GROUP-PARENT-OK/ && child && !seen { seen = 1 }
    END { exit seen ? 0 : 1 }
' "$LOG"; then
    problem="M4-GROUP-PARENT-OK appeared before any M4-GROUP-CHILD-OK (group word was not ordered)"
fi

if [ -n "$problem" ]; then
    echo "FAIL: M4.3 live group probe: $problem" >&2
    echo "  child=$child parent=$parent fail=$fail clone_line=$clone_line group_line=$group_line" >&2
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

echo "OK: M4.3 live group probe: child=$child parent=$parent fail=0 clone_lines=$clone_line group_lines=$group_line"
if [ "$KEEP" = "1" ] || [ -n "$REPLAY" ]; then
    echo "    serial log: $LOG"
fi
exit 0
