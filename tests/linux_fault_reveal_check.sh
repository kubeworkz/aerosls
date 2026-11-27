#!/usr/bin/env bash
# linux_fault_reveal_check.sh — E7 increment 34 (P1-C): the REVEAL half of an
# unmask inside a trap-delivered handler, end to end on a boot.
#
# Increment 33 gave that window the handler's own mask, so rt_sigprocmask
# there reports the right set. An unmask has a second half: a signal that was
# BLOCKED and is now UNBLOCKED must be REVEALED — discarded, or its default
# action run inside the call, where Linux's raise() dies on the unmask. The
# host test (tests/linux_sig_mask_host_test.c) proves the decision — which
# masks free a bit, and that only those spend a probe. This script proves the
# WIRING on the real machine:
#
#   e7.fault.reveal.0   SIGTERM is blocked (a forwarded 14 the kernel now also
#                       records), then pended by tkill while blocked, then a
#                       PROT_NONE read faults into .Lhandler, which UNBLOCKS
#                       SIGTERM. The sidecar's reveal ladder must decide that
#                       the pending signal terminates — inside that call.
#
#                       -> "[E7] fault handler unmask: forwarding the reveal"
#                       -> "sig=15 default-action -> kill" with num=0000000E,
#                          between this arm's set-up and any E7RV-AFTER
#                       -> the task is gone: no E7RV-AFTER, no E7RV-OK
#
# Why those markers are a proof and not a claim:
#   - E7RV-AFTER is written by the HANDLER, one instruction after the unblock
#     returns. It is the exact shape of the old gap: the mask moved, nothing
#     was revealed, and the pending SIGTERM waited for after the return.
#   - the kill line can only come from the reveal ladder: nothing else in this
#     arm raises SIGTERM, and no other arm in the boot blocks one before a
#     fault, so its position inside this arm's window is what attributes it.
#   - the earlier fault arms must still pass on the SAME boot (increment 32's
#     returning handler, increment 31's exiting one), which is what keeps this
#     a new capability rather than a change of behaviour — including for a
#     tkill-delivered handler, whose unblock still goes to the sidecar
#     unchanged.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

[ -s user/sidecar/fixtures/e7_fault_reveal_probe.flat ] \
    && sok "the revealing arm's flat payload is present" \
    || sbad "user/sidecar/fixtures/e7_fault_reveal_probe.flat is missing/empty"
grep -qF 'E7RV-ARM' user/sidecar/fixtures/e7_fault_reveal_probe.S \
    && sok "the revealing arm carries its set-up marker" \
    || sbad "the fixture lost E7RV-ARM"
grep -qF 'E7RV-AFTER' user/sidecar/fixtures/e7_fault_reveal_probe.S \
    && sok "a handler that returns from its unblock names the gap (E7RV-AFTER)" \
    || sbad "the fixture lost its named failure marker"
grep -qF 'movq    $0x4000, 0(%rsp)' user/sidecar/fixtures/e7_fault_reveal_probe.S \
    && sok "the arm uses SIGTERM's bit, both to block it and to free it" \
    || sbad "the fixture no longer names SIGTERM's bit"
grep -qF 'mov     $1, %edi                /* SIG_UNBLOCK' \
     user/sidecar/fixtures/e7_fault_reveal_probe.S \
    && sok "the handler's call is SIG_UNBLOCK — the shape that owes a reveal" \
    || sbad "the handler no longer unblocks"
grep -qF 'crate::e7_thread_death::spawn_fault_reveal' user/sidecar/src/entry.rs \
    && sok "the sidecar spawns the revealing arm through the real create" \
    || sbad "entry.rs no longer spawns the revealing arm"
grep -qF 'int linux_sig_mask_reveals(uint64_t old, uint64_t new_mask);' kernel/linux_abi.h \
    && sok "the kernel names when an unmask frees a bit" \
    || sbad "kernel/linux_abi.h no longer declares the reveal predicate"
grep -qF 'return (old & ~new_mask) != 0;' kernel/linux_abi.c \
    && sok "…and computes it from what LEFT the mask (old & ~new)" \
    || sbad "the reveal predicate is gone or inverted"
grep -qF 'largs[5] = LINUX_SIG_REVEAL_TAG;' kernel/syscall_dispatch.c \
    && sok "the forward is the tagged reveal probe, not the child's call" \
    || sbad "the dispatch no longer tags the probe"
grep -qF 'if (rax == 0) pd->linux_sig_mask = nm;' kernel/chan.c \
    && sok "the kernel commits the mask only on the sidecar's consent" \
    || sbad "kernel/chan.c no longer commits the staged mask on consent"
grep -qF 'if args[5] == SIG_MASK_REVEAL_TAG {' user/sidecar/src/linux.rs \
    && sok "the sidecar has the probe's arm" \
    || sbad "the sidecar no longer recognises the probe"

[ "$src_fails" -eq 0 ] || { echo "linux_fault_reveal_check: $src_fails source failure(s)"; exit 1; }

[ -f sls_operating_system.iso ] || {
    echo "ABORT: sls_operating_system.iso not found — run 'make x86-iso' first." >&2
    exit 2
}

W="$(mktemp -d)"
SER="$W/serial"
IMG="$W/disk.img"
LOG="$W/boot.log"
qemu-img create -f raw "$IMG" 10G >/dev/null 2>&1
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
cat "$SER.out" > "$LOG" &
CATPID=$!

ACCEL="${QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then
        ACCEL="-accel kvm"
    else
        ACCEL="-accel tcg,thread=multi"
    fi
fi

qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

qemu-system-x86_64 -cdrom sls_operating_system.iso \
    -drive id=disk,file="$IMG",if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -display none -m 4G -smp 4 -boot d -no-reboot \
    $ACCEL \
    -serial pipe:"$SER" 2>"$W/qemu.err" &
QPID=$!

cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

saw=0
for _ in $(seq 1 480); do
    if grep -aqF 'E7RV-ARM' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
if [ "$saw" -eq 0 ]; then
    echo "FAIL: the revealing arm never reached its set-up marker in 480s"
    qemu_err
    tail -40 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7RV_LOG:-/tmp/linux_fault_reveal_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7RV_LOG:-/tmp/linux_fault_reveal_boot.log}"
    exit 1
fi
sleep 10

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

grep -aqF 'E7RV-FAIL' "$LOG" && bad "the revealing arm refused its own set-up (E7RV-FAIL)" \
    || ok "the revealing arm's set-up was accepted"
grep -aqF 'E7RV-AFTER' "$LOG" \
    && bad "the unblock returned without revealing (E7RV-AFTER): the pending SIGTERM waited for after the return" \
    || ok "the unblock never returned (no E7RV-AFTER) — the reveal ran inside the handler"
grep -aqF 'E7RV-OK' "$LOG" \
    && bad "the handler returned (E7RV-OK): nothing was revealed inside the call" \
    || ok "the handler never returned (no E7RV-OK)"
grep -aqF '[E7] fault handler unmask: forwarding the reveal, mask' "$LOG" \
    && ok "the kernel forwarded the tagged reveal probe for the handler's unblock" \
    || bad "no '[E7] fault handler unmask: forwarding the reveal' line — the unblock was answered locally, so nothing could reveal"

# The positive: the sidecar's own reveal ladder decided, inside this call, that
# the pending signal terminates. It must sit after this arm's set-up, which is
# what attributes the kill to this window rather than to another arm.
if awk '/E7RV-ARM/ { a = NR }
        /sig=15 default-action -> kill/ { if (a && !k) k = NR }
        END { exit !(a && k && a < k) }' "$LOG" 2>/dev/null; then
    ok "the pending SIGTERM was REVEALED inside the handler (sig=15 default-action -> kill, after E7RV-ARM)"
else
    bad "no 'sig=15 default-action -> kill' line after E7RV-ARM — the reveal ladder never ran for this handler's unmask"
fi
grep -aqF 'num=0000000E child=e7.fault.reveal.0' "$LOG" \
    && ok "the line names the 14 this handler issued (num=0000000E, this arm's child)" \
    || bad "the kill line does not name this arm's rt_sigprocmask"

grep -aqE '\[E7\] fault sig 11 \(code [12]\) addr 0x[0-9a-f]+ -> handler [0-9a-f]+, frame [0-9a-f]+' "$LOG" \
    && ok "the #PF was delivered to a handler (not a kill)" \
    || bad "no '[E7] fault sig 11 … -> handler …' line"
grep -aqF '[E7] fault-reveal: spaw' "$LOG" \
    && ok "the arm was spawned through the sidecar's real create path" \
    || bad "no '[E7] fault-reveal: spaw…' line"

# …and the arms that must not change still pass on this same boot.
grep -aqF 'E7FR-OK' "$LOG" \
    && ok "increment 32's returning handler still resumes its faulting load" \
    || bad "E7FR-OK is absent: the fault handler's return regressed"
grep -aqF 'E7FH-OK' "$LOG" \
    && ok "increment 31's handler still runs and writes E7FH-OK" \
    || bad "E7FH-OK is absent: the increment-31 arm regressed"

echo ""
if [ "$fails" -eq 0 ]; then
    echo "linux_fault_reveal_check: 0 failure(s)"
    exit 0
fi
echo "linux_fault_reveal_check: $fails failure(s)"
cp "$LOG" "${E7RV_LOG:-/tmp/linux_fault_reveal_boot.log}" 2>/dev/null && \
    echo "boot log kept at ${E7RV_LOG:-/tmp/linux_fault_reveal_boot.log}"
exit 1
