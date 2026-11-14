#!/usr/bin/env bash
# tests/m4_exit_probe_check.sh — live QEMU gate for M4.5 identity and exit.
#
# GUARD-KIND: runtime (needs the built M4.5 ISO + QEMU)
#
# The fixture (user/sidecar/fixtures/m4_exit_probe.S) is a Linux task
# spawned by the initial POSIX sidecar. It proves:
#   - the leader's getpid == gettid;
#   - a clone child's gettid is its real TID (ptid) and differs from TGID;
#   - tkill reaches that TID by its real value;
#   - set_tid_address returns the TID, not the user pointer;
#   - exit(60) ends only the child: the kernel zeroes clear_child_tid and
#     the sidecar's wake releases the parent's FUTEX_WAIT (M4-JOIN-OK);
#   - exit_group(231) sweeps both remaining group threads and proves the
#     group's live count reached zero: the kernel's
#     `[PROC] M4.5 group sweep ... killed=2 live=0` and the sidecar's
#     `group swept=2 dead=2 live=0` are the sweep evidence.
#   - no group thread executes after the sweep. The fixture's C2 spins on
#     the leader's clear-tid word and faults if it sees the sweep's zero,
#     so a `[PROC] PID ... 'm4.exit.probe.0.tN' exited` or deferred-teardown
#     line after the sweep is a failure, not a pass.
#
# Markers:
#   M4-CHILD-OK, M4-IDENTITY-OK, M4-TKILL-OK, M4-JOIN-OK,
#   M4-EXITGROUP-CHILD-OK, M4-EXITGROUP-BEFORE ; failures M4-EXIT-FAIL and
#   M4-EXITGROUP-AFTER (exit_group returned).
#
# Environment:
#   M4_EXIT_ISO          image to boot       (default: sls_m4_exit.iso)
#   QEMU                 emulator command    (default: qemu-system-x86_64)
#   M4_EXIT_WINDOW_S     seconds to observe  (default: 120)
#   M4_EXIT_POST_SWEEP_S seconds to watch after the sweep (default: 3)
#   M4_EXIT_KEEP         keep the serial log on success (default: 0)
#   M4_EXIT_LOG          serial log path     (default: /tmp/m4_exit_probe.$$.log)
#   M4_EXIT_REPLAY       validate an existing log instead of booting QEMU
#
# Build:
#   make x86-iso M4_EXIT_PROBE=1 X86_ISO=sls_m4_exit.iso
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${M4_EXIT_ISO:-sls_m4_exit.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
WINDOW_S="${M4_EXIT_WINDOW_S:-120}"
POST_SWEEP_S="${M4_EXIT_POST_SWEEP_S:-3}"
REPLAY="${M4_EXIT_REPLAY:-}"
KEEP="${M4_EXIT_KEEP:-0}"
LOG="${M4_EXIT_LOG:-/tmp/m4_exit_probe.$$.log}"
QERR="${LOG}.qemuerr"
QPID=""
STARTED="0"

case "$WINDOW_S" in
    ''|*[!0-9]*|0)
        echo "ABORT: M4_EXIT_WINDOW_S must be a positive integer (got '$WINDOW_S')" >&2
        exit 2
        ;;
esac
case "$POST_SWEEP_S" in
    ''|*[!0-9]*)
        echo "ABORT: M4_EXIT_POST_SWEEP_S must be a non-negative integer (got '$POST_SWEEP_S')" >&2
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
        echo "ABORT: M4_EXIT_REPLAY is not a file: $REPLAY" >&2
        exit 2
    }
    LOG="$REPLAY"
    QERR=""
else
    [ -f "$ISO" ] || {
        echo "ABORT: missing M4.5 ISO '$ISO' (build with: make x86-iso M4_EXIT_PROBE=1 X86_ISO=$ISO)" >&2
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
    swept_at=0
    while kill -0 "$QPID" 2>/dev/null; do
        fail=$(count_fixed 'M4-EXIT-FAIL')
        after=$(count_fixed 'M4-EXITGROUP-AFTER')
        before=$(count_fixed 'M4-EXITGROUP-BEFORE')
        swept=$(count_fixed 'killed=2 live=0')
        now=$(date +%s)
        if [ "$fail" -gt 0 ] || [ "$after" -gt 0 ]; then
            sleep 1
            break
        fi
        if [ "$before" -gt 0 ] && [ "$swept" -gt 0 ] && [ "$swept_at" -eq 0 ]; then
            swept_at=$now
        fi
        if [ "$swept_at" -gt 0 ] && [ $((now - swept_at)) -ge "$POST_SWEEP_S" ]; then
            break
        fi
        if [ "$now" -ge "$deadline" ]; then
            break
        fi
        sleep 1
    done
    if kill -0 "$QPID" 2>/dev/null; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    QPID=""
fi

fail=$(count_fixed 'M4-EXIT-FAIL')
after=$(count_fixed 'M4-EXITGROUP-AFTER')
child=$(count_fixed 'M4-CHILD-OK')
identity=$(count_fixed 'M4-IDENTITY-OK')
tkill=$(count_fixed 'M4-TKILL-OK')
join=$(count_fixed 'M4-JOIN-OK')
group_child=$(count_fixed 'M4-EXITGROUP-CHILD-OK')
before=$(count_fixed 'M4-EXITGROUP-BEFORE')
clone_line=$(count_fixed '[PROC] M4.2 clone:')
sweep_line=$(count_fixed '[PROC] M4.5 group sweep')
swept_two=$(count_fixed 'killed=2 live=0')
side_swept=$(count_fixed 'group swept=2 dead=2 live=0')
sweep_failed=$(count_fixed 'group sweep FAILED')
thread_exit=$(count_fixed 'exit status=')
group_exit=$(count_fixed 'exit_group status=')
post_probe=$(awk '
    /\[PROC\] M4\.5 group sweep .*killed=.*live=0/ { armed = 1; next }
    armed && /m4\.exit\.probe\.0\.t[0-9]+/ && /(exited|deferred teardown)/ { n++ }
    END { print n + 0 }
' "$LOG" 2>/dev/null || printf '0\n')

problem=""
if [ "$fail" -gt 0 ]; then
    problem="serial log contains $fail M4-EXIT-FAIL marker(s)"
elif [ "$after" -gt 0 ]; then
    problem="exit_group returned ($after M4-EXITGROUP-AFTER marker(s)); the caller was not swept"
elif [ "$child" -lt 1 ]; then
    problem="no M4-CHILD-OK marker (C1 identity/self-tkill arm did not complete)"
elif [ "$identity" -lt 1 ]; then
    problem="no M4-IDENTITY-OK marker (real getpid/gettid identity did not match)"
elif [ "$tkill" -lt 1 ]; then
    problem="no M4-TKILL-OK marker (tkill did not reach C1 by its real TID)"
elif [ "$join" -lt 1 ]; then
    problem="no M4-JOIN-OK marker (clear_child_tid was not zeroed and/or no wake)"
elif [ "$group_child" -lt 1 ]; then
    problem="no M4-EXITGROUP-CHILD-OK marker (second thread never ran)"
elif [ "$before" -lt 1 ]; then
    problem="no M4-EXITGROUP-BEFORE marker (leader never reached exit_group)"
elif [ "$clone_line" -lt 2 ]; then
    problem="fewer than two kernel clone lines (both threads were not created)"
elif [ "$sweep_line" -lt 1 ] || [ "$swept_two" -lt 1 ]; then
    problem="no [PROC] M4.5 group sweep ... killed=2 live=0 line (the sweep did not prove the group dead)"
elif [ "$sweep_failed" -gt 0 ]; then
    problem="sidecar logged $sweep_failed group-sweep failure(s); a live member was not reaped"
elif [ "$side_swept" -lt 1 ]; then
    problem="no sidecar 'group swept=2 dead=2 live=0' line (records were not reaped only after death)"
elif [ "$post_probe" -gt 0 ]; then
    problem="$post_probe group-thread execution/deferred-teardown line(s) after the sweep (a sibling ran after exit_group)"
elif [ "$thread_exit" -lt 1 ]; then
    problem="no sidecar thread-exit line (exit(60) did not take the single-thread path)"
elif [ "$group_exit" -lt 1 ]; then
    problem="no sidecar exit_group line"
elif ! awk '
    /M4-CHILD-OK/ { child = NR }
    /M4-JOIN-OK/ && child && !join_seen { join_seen = 1 }
    /M4-EXITGROUP-CHILD-OK/ && join_seen && !group_child { group_child = 1 }
    /M4-EXITGROUP-BEFORE/ && group_child { before = 1 }
    /\[PROC\] M4\.5 group sweep/ && before { swept = 1 }
    END { exit (join_seen && group_child && before && swept) ? 0 : 1 }
' "$LOG"; then
    problem="marker order was not child -> join -> second child -> exit_group -> sweep"
fi

if [ -n "$problem" ]; then
    echo "FAIL: M4.5 live identity/exit probe: $problem" >&2
    echo "  child=$child identity=$identity tkill=$tkill join=$join group_child=$group_child before=$before sweep=$sweep_line killed2_dead=$swept_two side_swept=$side_swept sweep_failed=$sweep_failed post_probe=$post_probe fail=$fail after=$after" >&2
    echo "  clone_lines=$clone_line thread_exit=$thread_exit group_exit=$group_exit" >&2
    if [ -f "$LOG" ]; then
        echo "  last 60 serial lines ($LOG):" >&2
        tail -60 "$LOG" >&2
    else
        echo "  serial log was not created: $LOG" >&2
    fi
    if [ -n "$QERR" ] && [ -s "$QERR" ]; then
        echo "  qemu stderr:" >&2
        tail -20 "$QERR" >&2
    fi
    exit 1
fi

echo "OK: M4.5 live identity/exit probe: child=$child identity=$identity tkill=$tkill join=$join group_child=$group_child before=$before sweep=$sweep_line killed2_dead=$swept_two side_swept=$side_swept post_probe=0 fail=0"
echo "    clone_lines=$clone_line thread_exit=$thread_exit group_exit=$group_exit"
if [ "$KEEP" = "1" ] || [ -n "$REPLAY" ]; then
    echo "    serial log: $LOG"
fi
exit 0
