#!/usr/bin/env bash
# tests/m4_hardening_probe_check.sh — live QEMU gate for M4.8 hardening,
# extended by M4.11's running-sibling handler and group-stop broadcast.
#
# GUARD-KIND: runtime (needs the built M4.8 ISO + QEMU, and for the build arm
# cargo, the x86_64-unknown-none target and the kernel's X86_CC/X86_LD)
#
# The fixture (tools/m4_hardening_gate, a static non-PIE musl ELF) proves:
#   - clone3(435) creates two threads through the 88-byte clone_args copy;
#   - FUTEX_REQUEUE moves a parked parent from A to B and a wake releases it;
#   - set_robust_list + thread exit ORs FUTEX_OWNER_DIED and wakes the waiter;
#   - kill(TGID, SIGUSR1) delivers a recorded handler to a parked sibling,
#     and kill(TGID, SIGSTOP)/SIGCONT withholds then resumes its reply;
#     the sidecar's -EINTR trailer contract is pinned by the focused host
#     test in user/sidecar/src/linux.rs (M4.10);
#   - tgkill(234) delivers a recorded handler to a running sibling through
#     SYS_SLS_LINUX_SIGNAL_TASK's direct ring3_ctx install (M4.11);
#   - the same SIGSTOP broadcast holds every non-caller sibling: the parked
#     one keeps its reply and the running one is held by the kernel, with
#     SIGCONT releasing both (M4.11);
#   - tgkill(234) kills a parked sibling by TGID/TID and releases its join;
#   - kill(TGID, SIGTERM) folds into the existing exit_group sweep.
#
# Markers: M4-CLONE3-OK, M4-REQUEUE-OK, M4-REQUEUE-PARENT-OK,
# M4-GROUP-HANDLER-PASS, M4-RUNNING-HANDLER-PASS,
# M4-GROUP-STOP-BROADCAST-PASS, M4-GROUP-STOP-PASS, M4-ROBUST-OK,
# M4-TGKILL-OK, M4-HARDENING-PASS, M4-GROUP-KILL-SENT.
# Failures: M4-HARDENING-FAIL. A post-sweep marker is a failure.
#
# Environment:
#   M4_HARDENING_ISO       image to boot       (default: sls_m4_hardening_e3.iso)
#   M4_HARDENING_BUILD     build the ISO first (default: 1)
#   M4_HARDENING_WINDOW_S  seconds to observe  (default: 150)
#   M4_HARDENING_REPLAY    validate an existing log instead of booting
#   M4_HARDENING_LOG       serial log path     (default: /tmp/m4_hardening.$$.log)
#   QEMU                   emulator command    (default: qemu-system-x86_64)
#
# Build (the E3 gate profile is required: the standard boot's irqtest kills
# the POSIX sidecar):
#   make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_HARDENING_PROBE=1 \
#       SIDECAR_E3_CPIO=sidecars_m4_hardening_e3.cpio \
#       X86_E3_ISO=sls_m4_hardening_e3.iso
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.
set -u

cd "$(dirname "$0")/.." || exit 1

ISO="${M4_HARDENING_ISO:-sls_m4_hardening_e3.iso}"
BUILD="${M4_HARDENING_BUILD:-1}"
QEMU="${QEMU:-qemu-system-x86_64}"
WINDOW_S="${M4_HARDENING_WINDOW_S:-150}"
REPLAY="${M4_HARDENING_REPLAY:-}"
LOG="${M4_HARDENING_LOG:-/tmp/m4_hardening.$$.log}"
QERR="${LOG}.qemuerr"
QPID=""

case "$WINDOW_S" in
    ''|*[!0-9]*|0)
        echo "ABORT: M4_HARDENING_WINDOW_S must be a positive integer (got '$WINDOW_S')" >&2
        exit 2
        ;;
esac

cleanup() {
    local rc=$?
    if [ -n "$QPID" ]; then
        kill "$QPID" 2>/dev/null || true
        wait "$QPID" 2>/dev/null || true
    fi
    if [ -z "$REPLAY" ] && [ "${M4_HARDENING_KEEP:-0}" != "1" ]; then
        rm -f "$LOG" "$QERR"
    fi
    return "$rc"
}
trap cleanup EXIT
abort() { echo "ABORT: $*" >&2; exit 2; }

count_fixed() {
    local n
    n=$(grep -aFc -- "$1" "$LOG" 2>/dev/null || true)
    printf '%s\n' "${n:-0}"
}

if [ -n "$REPLAY" ]; then
    [ -f "$REPLAY" ] || { echo "ABORT: M4_HARDENING_REPLAY is not a file: $REPLAY" >&2; exit 2; }
    LOG="$REPLAY"
    QERR=""
else
    # ── live prerequisites: absent is a skip (2), not a failure (1) ──────
    # The static clauses above have already run, so a real regression still
    # exits 1 before this point. run_checks.sh turns a GUARD-KIND: runtime
    # guard's exit 2 into an owed skip instead of failing the job;
    # kernel-guards builds the probe ISO and runs the gate. Each message names
    # what to install or which variable to set, because the skip prints it.
    if [ "$BUILD" = "1" ]; then
        command -v make >/dev/null 2>&1 || \
            abort "make not found — the build arm runs 'make x86-iso-e3' (or set M4_HARDENING_BUILD=0 with a prebuilt ISO)"
        command -v cargo >/dev/null 2>&1 || \
            abort "cargo not found — the probe ISO cross-builds the six sidecars for x86_64-unknown-none"
        sysroot=$(rustc --print sysroot 2>/dev/null) || sysroot=""
        [ -n "$sysroot" ] && [ -d "$sysroot/lib/rustlib/x86_64-unknown-none" ] || \
            abort "the x86_64-unknown-none rust-std component is not installed (rustup target add x86_64-unknown-none)"
        cc=${X86_CC:-gcc}
        ld=${X86_LD:-ld}
        command -v "$cc" >/dev/null 2>&1 || \
            abort "the kernel compiler '$cc' is not on PATH — install gcc, or set M4_HARDENING_BUILD=0 with a prebuilt ISO"
        command -v "$ld" >/dev/null 2>&1 || \
            abort "the kernel linker '$ld' is not on PATH — install binutils, or set M4_HARDENING_BUILD=0 with a prebuilt ISO"
    fi
    command -v "${QEMU%% *}" >/dev/null 2>&1 || \
        abort "missing QEMU executable '$QEMU' — the gate checkpoints in one boot and restores in a second (install qemu-system-x86, or set M4_HARDENING_BUILD=0 after building the ISO)"

    if [ "$BUILD" = "1" ]; then
        echo "=== build: x86-iso-e3 M4_HARDENING_PROBE=1 (gate profile, X86_CC=gcc X86_LD=ld) ==="
        # Keep the probe-specific feature out of the tracked default archive:
        # this E3 pack and its stamps are a separate, disposable artifact.
        make X86_CC="${X86_CC:-gcc}" X86_LD="${X86_LD:-ld}" \
            x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate \
            M4_HARDENING_PROBE=1 \
            SIDECAR_E3_CPIO=sidecars_m4_hardening_e3.cpio \
            X86_E3_ISO="$ISO" || {
            echo "FAIL: the M4.8 E3 image did not build" >&2
            exit 1
        }
    fi
    [ -f "$ISO" ] || {
        echo "ABORT: missing M4.8 ISO '$ISO' (build with: make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_HARDENING_PROBE=1 SIDECAR_E3_CPIO=sidecars_m4_hardening_e3.cpio X86_E3_ISO=$ISO)" >&2
        exit 2
    }
    command -v "$QEMU" >/dev/null 2>&1 || {
        echo "ABORT: missing QEMU executable '$QEMU'" >&2
        exit 2
    }

    grep -qF 'm4_hardening_gate.elf' user/sidecar/src/m4_hardening_probe.rs || {
        echo "ABORT: the probe does not embed the hardening fixture" >&2
        exit 2
    }
    grep -qF 'm4_hardening_probe' user/sidecar/src/lib.rs || {
        echo "ABORT: the hardening probe module is not compiled" >&2
        exit 2
    }

    rm -f "$LOG" "$QERR"
    "$QEMU" \
        -cdrom "$ISO" \
        -m 4G -smp 4 -accel tcg \
        -display none -monitor none -no-reboot -boot d \
        -serial "file:$LOG" \
        >/dev/null 2>"$QERR" &
    QPID=$!

    deadline=$(( $(date +%s) + WINDOW_S ))
    while kill -0 "$QPID" 2>/dev/null; do
        fail=$(count_fixed 'M4-HARDENING-FAIL')
        pass=$(count_fixed 'M4-HARDENING-PASS')
        group=$(count_fixed '[PROC] M4.5 group sweep')
        if [ "$fail" -gt 0 ] || [ "$pass" -gt 0 ] && [ "$group" -gt 0 ]; then
            sleep 1
            break
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
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

fail=$(count_fixed 'M4-HARDENING-FAIL')
pass=$(count_fixed 'M4-HARDENING-PASS')
after=$(count_fixed 'M4-HARDENING-AFTER-GROUP-KILL')
clone3=$(count_fixed 'M4-CLONE3-OK=')
clone3b=$(count_fixed 'M4-CLONE3-B-OK=')
requeue=$(count_fixed 'M4-REQUEUE-OK')
parent=$(count_fixed 'M4-REQUEUE-PARENT-OK')
robust=$(count_fixed 'M4-ROBUST-OK=')
group_handler=$(count_fixed 'M4-GROUP-HANDLER-PASS')
run_handler=$(count_fixed 'M4-RUNNING-HANDLER-PASS')
run_handler_sig=$(count_fixed 'sibling sig=12 handler -> running frame')
group_stop_broadcast=$(count_fixed 'M4-GROUP-STOP-BROADCAST-PASS')
group_stop_broadcast_log=$(count_fixed 'M4.11 group stop broadcast targets=2')
group_stop_held=$(count_fixed 'sibling sig=19 -> stop (kernel hold)')
group_stop=$(count_fixed 'M4-GROUP-STOP-PASS')
tgkill=$(count_fixed 'M4-TGKILL-OK')
group_sent=$(count_fixed 'M4-GROUP-KILL-SENT')
sweep=$(count_fixed '[PROC] M4.5 group sweep')
side_sweep=$(count_fixed 'group swept=')
clone_lines=$(count_fixed '[PROC] M4.2 clone:')
sibling_sig=$(count_fixed 'sibling sig=15 -> kill')
group_handler_sig=$(count_fixed 'sibling sig=10 handler -> frame')
group_stop_sig=$(count_fixed 'sibling sig=19 -> stop (pending reply held)')
group_cont_sig=$(count_fixed 'SIGCONT -> sibling resumed')
group_sig=$(count_fixed 'group sig=15 -> exit_group sweep')
robust_bit=$(count_fixed 'M4-ROBUST-BIT=1073741824')

problem=""
if [ "$fail" -gt 0 ]; then
    problem="serial log contains $fail M4-HARDENING-FAIL marker(s)"
elif [ "$after" -gt 0 ]; then
    problem="a task executed after the group kill"
elif [ "$clone3" -lt 1 ]; then
    problem="no M4-CLONE3-OK marker (clone3 did not create the first thread)"
elif [ "$clone3b" -lt 1 ]; then
    problem="no M4-CLONE3-B-OK marker (clone3 did not create the robust-list thread)"
elif [ "$requeue" -lt 1 ] || [ "$parent" -lt 1 ]; then
    problem="requeue did not move and release the parent"
elif [ "$group_handler" -lt 1 ]; then
    problem="no M4-GROUP-HANDLER-PASS marker (handler not delivered to the parked sibling)"
elif [ "$group_handler_sig" -lt 1 ]; then
    problem="no sidecar sibling signal=10 handler line"
elif [ "$run_handler" -lt 1 ]; then
    problem="no M4-RUNNING-HANDLER-PASS marker (running sibling handler did not run)"
elif [ "$run_handler_sig" -lt 1 ]; then
    problem="no sidecar running-sibling handler frame line"
elif [ "$group_stop" -lt 1 ]; then
    problem="no M4-GROUP-STOP-PASS marker (stop/SIGCONT did not resume the sibling)"
elif [ "$group_stop_broadcast" -lt 1 ]; then
    problem="no M4-GROUP-STOP-BROADCAST-PASS marker (running sibling did not resume)"
elif [ "$group_stop_broadcast_log" -lt 1 ]; then
    problem="no sidecar group-stop broadcast targets=2 line"
elif [ "$group_stop_held" -lt 1 ]; then
    problem="no sidecar kernel-hold line for the running sibling"
elif [ "$group_stop_sig" -lt 1 ] || [ "$group_cont_sig" -lt 1 ]; then
    problem="no sidecar stop-latch/SIGCONT resume lines"
elif [ "$robust" -lt 1 ] || [ "$robust_bit" -lt 1 ]; then
    problem="no robust owner-died marker (set_robust_list/exit walk did not run)"
elif [ "$tgkill" -lt 1 ]; then
    problem="no M4-TGKILL-OK marker (sibling signal did not reap/join)"
elif [ "$pass" -lt 1 ]; then
    problem="no M4-HARDENING-PASS marker"
elif [ "$group_sent" -lt 1 ]; then
    problem="no M4-GROUP-KILL-SENT marker"
elif [ "$sweep" -lt 1 ]; then
    problem="no [PROC] M4.5 group sweep line"
elif [ "$side_sweep" -lt 1 ]; then
    problem="no sidecar group-sweep line"
elif [ "$clone_lines" -lt 2 ]; then
    problem="fewer than two kernel clone lines"
elif [ "$sibling_sig" -lt 1 ]; then
    problem="no sidecar sibling-signal kill line"
elif [ "$group_sig" -lt 1 ]; then
    problem="no sidecar group-signal fold line"
elif ! awk '
    /M4-CLONE3-OK=/ { c = NR }
    /M4-REQUEUE-OK/ && c && !r { r = NR }
    /M4-GROUP-HANDLER-PASS/ && r && !h { h = NR }
    /M4-RUNNING-HANDLER-PASS/ && h && !rh { rh = NR }
    /M4-GROUP-STOP-BROADCAST-PASS/ && rh && !rb { rb = NR }
    /M4-GROUP-STOP-PASS/ && rb && !k { k = NR }
    /M4-ROBUST-OK=/ && k && !b { b = NR }
    /M4-TGKILL-OK/ && b && !t { t = NR }
    /M4-HARDENING-PASS/ && t && !p { p = NR }
    /M4-GROUP-KILL-SENT/ && p && !g { g = NR }
    /\[PROC\] M4\.5 group sweep/ && g && !s { s = NR }
    END { exit (c && r && h && rh && rb && k && b && t && p && g && s) ? 0 : 1 }
' "$LOG"; then
    problem="marker order was not clone3 -> requeue -> group-handler -> running-handler -> group-stop-broadcast -> group-stop -> robust -> tgkill -> pass -> group-kill -> sweep"
fi

if [ -n "$problem" ]; then
    echo "FAIL: M4.8 live hardening probe: $problem" >&2
    echo "  fail=$fail clone3=$clone3 clone3b=$clone3b requeue=$requeue parent=$parent group_handler=$group_handler group_stop=$group_stop robust=$robust bit=$robust_bit tgkill=$tgkill pass=$pass group_sent=$group_sent sweep=$sweep side_sweep=$side_sweep after=$after" >&2
    echo "  clone_lines=$clone_lines sibling_sig=$sibling_sig group_handler_sig=$group_handler_sig run_handler=$run_handler run_handler_sig=$run_handler_sig group_stop_broadcast=$group_stop_broadcast group_stop_broadcast_log=$group_stop_broadcast_log group_stop_held=$group_stop_held group_stop_sig=$group_stop_sig group_cont_sig=$group_cont_sig group_sig=$group_sig" >&2
    if [ -f "$LOG" ]; then
        echo "  last 80 serial lines ($LOG):" >&2
        tail -80 "$LOG" >&2
    fi
    if [ -n "$QERR" ] && [ -s "$QERR" ]; then
        echo "  qemu stderr:" >&2
        tail -20 "$QERR" >&2
    fi
    exit 1
fi

echo "OK: M4.8 live hardening probe: clone3=$clone3 clone3b=$clone3b requeue=$requeue parent=$parent group_handler=$group_handler group_stop=$group_stop robust=$robust bit=$robust_bit tgkill=$tgkill pass=$pass group_sent=$group_sent sweep=$sweep"
echo "    clone_lines=$clone_lines sibling_sig=$sibling_sig group_handler_sig=$group_handler_sig run_handler=$run_handler run_handler_sig=$run_handler_sig group_stop_broadcast=$group_stop_broadcast group_stop_broadcast_log=$group_stop_broadcast_log group_stop_held=$group_stop_held group_stop_sig=$group_stop_sig group_cont_sig=$group_cont_sig group_sig=$group_sig after=0 fail=0"
if [ -n "$REPLAY" ]; then
    echo "    serial log: $LOG"
fi
exit 0
