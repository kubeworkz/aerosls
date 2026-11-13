#!/usr/bin/env bash
# tests/m1_net_probe_check.sh -- live QEMU gate for the M1 network probe.
#
# GUARD-KIND: runtime (needs the built M1 ISO + QEMU)
#
# The M1 probe (user/sidecar/fixtures/m1_net_probe.S) runs inside the initial
# POSIX sidecar. A passing execution performs exactly:
#     socket=2  bind=1  listen=1  connect=1  accept=1
#     poll=1    sendto=2  recvfrom=2  shutdown=3  close=3
# and exits through `exit_group status=0`. The image runs the probe from the
# watchdog path, so more than one successful execution is normal. `M1-NET
# PASS` is printed several times per execution by the serial logger and is
# therefore NOT an execution counter; `exit_group status=0` is.
#
# This gate FAILS on:
#   * any `M1-NET FAIL` in the serial log,
#   * zero successful probe executions,
#   * any of socket/bind/listen/connect/accept/poll/sendto/shutdown/close
#     other than the per-execution constants above,
#   * recvfrom below 2 per execution (an EAGAIN retry may legitimately add
#     another recvfrom, so this is a floor rather than an exact count).
#
# Environment:
#   M1_ISO           image to boot       (default: sls_operating_system_m1net.iso)
#   QEMU             emulator command    (default: qemu-system-x86_64)
#   M1_GATE_WINDOW_S seconds to observe  (default: 150)
#   M1_GATE_KEEP     keep the serial log on success (default: 0)
#   M1_GATE_LOG      serial log path     (default: /tmp/m1_net_probe_gate.$$.log)
#   M1_GATE_REPLAY   validate an existing log instead of booting QEMU
#   M1_GATE_TOOTH    named test-only sabotage expected from the image:
#                    `bypass-connect` requires the kernel marker
#                    `[M1-TOOTH] bypass-connect`, a stage-5 probe failure,
#                    zero forwarded connects, and zero successful executions.
#                    The command exits 0 only when that red is observed.
#
# A normal image must contain no M1-TOOTH marker. If one appears, the ordinary
# gate fails and names it, so a tooth image cannot masquerade as a green run.
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${M1_ISO:-sls_operating_system_m1net.iso}"
QEMU="${QEMU:-qemu-system-x86_64}"
WINDOW_S="${M1_GATE_WINDOW_S:-150}"
REPLAY="${M1_GATE_REPLAY:-}"
KEEP="${M1_GATE_KEEP:-0}"
LOG="${M1_GATE_LOG:-/tmp/m1_net_probe_gate.$$.log}"
TOOTH="${M1_GATE_TOOTH:-}"
QERR="${LOG}.qemuerr"
QPID=""
STARTED="0"

case "$WINDOW_S" in
    ''|*[!0-9]*|0)
        echo "ABORT: M1_GATE_WINDOW_S must be a positive integer (got '$WINDOW_S')" >&2
        exit 2
        ;;
esac

case "$TOOTH" in
    ''|bypass-connect) ;;
    *)
        echo "ABORT: M1_GATE_TOOTH must be empty or bypass-connect (got '$TOOTH')" >&2
        exit 2
        ;;
esac

cleanup() {
    if [ -n "$QPID" ]; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    if [ "$KEEP" != "1" ] && [ -z "$REPLAY" ] && [ "$STARTED" = "1" ]; then
        rm -f "$LOG" "$QERR"
    fi
}
trap cleanup EXIT

count() {
    local n
    n=$(grep -ac -- "$1" "$LOG" 2>/dev/null || true)
    printf '%s\n' "${n:-0}"
}

count_fixed() {
    local n
    n=$(grep -aFc -- "$1" "$LOG" 2>/dev/null || true)
    printf '%s\n' "${n:-0}"
}

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || {
        echo "ABORT: M1_GATE_REPLAY is not a file: $REPLAY" >&2
        exit 2
    }
    LOG="$REPLAY"
    QERR=""
else
    [ -f "$ISO" ] || {
        echo "ABORT: missing M1 ISO '$ISO' (build with: make X86_CC=gcc X86_LD=ld x86-iso-e3 M1_NET_PROBE=1 X86_E3_ISO=$ISO)" >&2
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
        -netdev user,id=net0 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
        -serial "file:$LOG" \
        >/dev/null 2>"$QERR" &
    QPID=$!

    deadline=$(( $(date +%s) + WINDOW_S ))
    while kill -0 "$QPID" 2>/dev/null; do
        fails=$(count 'M1-NET FAIL')
        passes=$(count 'm1.net.probe.0 exit_group status=0')
        # Stop on the first failure, or once the watchdog has produced two
        # successful executions. A one-execution boot is still accepted below;
        # waiting the whole window would only make the guard slower.
        if [ "$fails" -gt 0 ] || [ "$passes" -ge 2 ]; then
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
    fi
    wait "$QPID" 2>/dev/null || true
    QPID=""
    sleep 1
fi

passes=$(count 'm1.net.probe.0 exit_group status=0')
fail_markers=$(count 'M1-NET FAIL')
socket=$(count 'num=00000029 child=m1.net.probe.0')
bind=$(count 'num=00000031 child=m1.net.probe.0')
listen=$(count 'num=00000032 child=m1.net.probe.0')
connect=$(count 'num=0000002A child=m1.net.probe.0')
accept=$(count 'num=0000002B child=m1.net.probe.0')
poll=$(count 'num=00000007 child=m1.net.probe.0')
sendto=$(count 'num=0000002C child=m1.net.probe.0')
recvfrom=$(count 'num=0000002D child=m1.net.probe.0')
shutdown=$(count 'num=00000030 child=m1.net.probe.0')
close=$(count 'num=00000003 child=m1.net.probe.0')
tooth_hits=$(count 'M1-TOOTH')
tooth_names=$(grep -ao '\[M1-TOOTH\] [a-z-]*' "$LOG" 2>/dev/null | sort -u | tr '\n' ' ' | sed 's/ *$//' || true)

expected_socket=$((passes * 2))
expected_bind=$passes
expected_listen=$passes
expected_connect=$passes
expected_accept=$passes
expected_poll=$passes
expected_sendto=$((passes * 2))
expected_recvfrom=$((passes * 2))
expected_shutdown=$((passes * 3))
expected_close=$((passes * 3))

if [ -n "$TOOTH" ]; then
    # The negative arm: the image must fail for the reason it was built to
    # fail, not merely fail. `exit_group status=5` is the probe's connect-stage
    # unwind (the probe exits with its stage number); a forwarded connect is
    # still visible as `[E7] num=0000002A ...`, so zero is the proof that the
    # sidecar was bypassed rather than broken.
    attempts=$(count 'm1.net.probe.0 exit_group status=5')
    tooth_exact=$(count_fixed "[M1-TOOTH] $TOOTH")
    expected_socket=$((attempts * 2))
    expected_bind=$attempts
    expected_listen=$attempts
    problem=""
    if [ "$tooth_exact" -lt 1 ]; then
        problem="expected named marker '[M1-TOOTH] $TOOTH' was absent"
    elif [ "$tooth_exact" -lt "$attempts" ]; then
        problem="named marker count $tooth_exact is below the $attempts stage-5 execution(s)"
    elif [ "$attempts" -lt 1 ]; then
        problem="no stage-5 probe exit (exit_group status=5) after the tooth"
    elif [ "$passes" -ne 0 ]; then
        problem="probe still passed $passes time(s) despite the tooth"
    elif [ "$fail_markers" -lt 1 ]; then
        problem="no 'M1-NET FAIL' marker after the tooth"
    elif [ "$connect" -ne 0 ]; then
        problem="connect was forwarded $connect time(s); the tooth did not bypass the sidecar"
    elif [ "$socket" -ne "$expected_socket" ]; then
        problem="socket coverage $socket, expected $expected_socket for $attempts tooth execution(s)"
    elif [ "$bind" -ne "$expected_bind" ]; then
        problem="bind coverage $bind, expected $expected_bind for $attempts tooth execution(s)"
    elif [ "$listen" -ne "$expected_listen" ]; then
        problem="listen coverage $listen, expected $expected_listen for $attempts tooth execution(s)"
    elif [ "$accept" -ne 0 ] || [ "$poll" -ne 0 ] || [ "$sendto" -ne 0 ] ||
         [ "$recvfrom" -ne 0 ] || [ "$shutdown" -ne 0 ] || [ "$close" -ne 0 ]; then
        problem="calls after the refused connect: accept=$accept poll=$poll sendto=$sendto recvfrom=$recvfrom shutdown=$shutdown close=$close"
    fi

    if [ -n "$problem" ]; then
        echo "FAIL: M1 sabotage tooth '$TOOTH' did not redden as named: $problem" >&2
        echo "  executions: passed=$passes fail_markers=$fail_markers stage5=$attempts tooth_marker=$tooth_exact" >&2
        echo "  coverage: socket=$socket bind=$bind listen=$listen connect=$connect accept=$accept poll=$poll sendto=$sendto recvfrom=$recvfrom shutdown=$shutdown close=$close" >&2
        if [ -f "$LOG" ]; then
            echo "  last 40 serial lines ($LOG):" >&2
            tail -40 "$LOG" >&2
        else
            echo "  serial log was not created: $LOG" >&2
        fi
        exit 1
    fi

    echo "OK: M1 sabotage tooth '$TOOTH' reddened by name: $attempts stage-5 execution(s), $fail_markers fail marker(s), $tooth_exact named marker(s), 0 forwarded connect(s), 0 pass(es)"
    echo "    coverage: socket=$socket bind=$bind listen=$listen connect=$connect accept=$accept poll=$poll sendto=$sendto recvfrom=$recvfrom shutdown=$shutdown close=$close"
    if [ "$KEEP" = "1" ]; then
        echo "    serial log: $LOG"
    fi
    exit 0
fi

problem=""
if [ "$tooth_hits" -gt 0 ]; then
    problem="serial log contains $tooth_hits 'M1-TOOTH' marker(s)${tooth_names:+: $tooth_names}"
elif [ "$fail_markers" -gt 0 ]; then
    problem="serial log contains $fail_markers 'M1-NET FAIL' marker(s)"
elif [ "$passes" -lt 1 ]; then
    problem="no successful m1.net.probe.0 execution (exit_group status=0)"
elif [ "$socket" -ne "$expected_socket" ]; then
    problem="socket coverage $socket, expected $expected_socket for $passes execution(s)"
elif [ "$bind" -ne "$expected_bind" ]; then
    problem="bind coverage $bind, expected $expected_bind for $passes execution(s)"
elif [ "$listen" -ne "$expected_listen" ]; then
    problem="listen coverage $listen, expected $expected_listen for $passes execution(s)"
elif [ "$connect" -ne "$expected_connect" ]; then
    problem="connect coverage $connect, expected $expected_connect for $passes execution(s)"
elif [ "$accept" -ne "$expected_accept" ]; then
    problem="accept coverage $accept, expected $expected_accept for $passes execution(s)"
elif [ "$poll" -ne "$expected_poll" ]; then
    problem="poll coverage $poll, expected $expected_poll for $passes execution(s)"
elif [ "$sendto" -ne "$expected_sendto" ]; then
    problem="sendto coverage $sendto, expected $expected_sendto for $passes execution(s)"
elif [ "$recvfrom" -lt "$expected_recvfrom" ]; then
    problem="recvfrom coverage $recvfrom, expected at least $expected_recvfrom for $passes execution(s)"
elif [ "$shutdown" -ne "$expected_shutdown" ]; then
    problem="shutdown coverage $shutdown, expected $expected_shutdown for $passes execution(s)"
elif [ "$close" -ne "$expected_close" ]; then
    problem="close coverage $close, expected $expected_close for $passes execution(s)"
fi

if [ -n "$problem" ]; then
    echo "FAIL: M1 live network probe: $problem" >&2
    echo "  executions: passed=$passes fail_markers=$fail_markers" >&2
    echo "  coverage: socket=$socket bind=$bind listen=$listen connect=$connect accept=$accept poll=$poll sendto=$sendto recvfrom=$recvfrom shutdown=$shutdown close=$close" >&2
    [ -n "$tooth_names" ] && echo "  sabotage marker(s): $tooth_names" >&2
    echo "  expected: socket=$expected_socket bind=$expected_bind listen=$expected_listen connect=$expected_connect accept=$expected_accept poll=$expected_poll sendto=$expected_sendto recvfrom>=$expected_recvfrom shutdown=$expected_shutdown close=$expected_close" >&2
    if [ -f "$LOG" ]; then
        echo "  last 40 serial lines ($LOG):" >&2
        tail -40 "$LOG" >&2
    else
        echo "  serial log was not created: $LOG" >&2
    fi
    if [ -n "$QERR" ] && [ -s "$QERR" ]; then
        echo "  qemu stderr:" >&2
        tail -20 "$QERR" >&2
    fi
    exit 1
fi

echo "OK: M1 live network probe: $passes execution(s); no M1-NET FAIL"
echo "    coverage: socket=$socket bind=$bind listen=$listen connect=$connect accept=$accept poll=$poll sendto=$sendto recvfrom=$recvfrom shutdown=$shutdown close=$close"
if [ "$KEEP" = "1" ] || [ -n "$REPLAY" ]; then
    echo "    serial log: $LOG"
fi
exit 0
