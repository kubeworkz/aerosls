#!/usr/bin/env bash
# linux_fault_abandon_check.sh — E7 increment 35 (P1-C): the frame a handler
# LEAVES instead of returning from, end to end on a boot.
#
# Increments 32–34 answer a trap handler's rt_sigprocmask out of the kernel's
# own mask copy, for as long as the handler is inside its frame. A handler
# that leaves by longjmp never sends the 15 that closes that window, and pops
# rsp above the frame instead. Increment 35 closes the window on the first 14
# made with rsp above the recorded frame: the mask goes back to the frame's
# own uc_sigmask (the pre-delivery set both ends still hold) and the frame is
# dropped. The host test (tests/linux_sig_mask_host_test.c) and clause AE pin
# the predicate and the placement; THIS script proves the wiring on the real
# machine:
#
#   e7.fault.abandon.0  SIGTERM is blocked (a forwarded 14, so both copies
#                       hold 0x4000 and the frame RECORDS it), then a
#                       PROT_NONE read faults into .Lhandler, which blocks
#                       SIGUSR1 (0x200 — an edit only the kernel's window
#                       sees), writes E7AB-LEAP and pops rsp back to the
#                       setjmp caller. The first 14 after that jump must
#                       close the window and be answered by the sidecar.
#
#                       -> "[E7] fault handler abandoned the frame at … ",
#                          "mask window closed at 4000"
#                       -> the query's oldset is 0x4000, not the dead
#                          handler's 0x4600 (E7AB-MASK if it is)
#                       -> the following rt_sigreturn(15) is -EINVAL, so the
#                          frame is gone (E7AB-RET if it restored, E7AB-LOOP
#                          if the faulting load ran again)
#                       -> E7AB-OK only when both hold
#
# Why those markers are a proof and not a claim:
#   - E7AB-MASK is the window left open: the kernel would answer the query
#     from its own copy, which the handler moved to 0x4600.
#   - E7AB-RET / E7AB-LOOP is the frame not dropped: 15 would restore the
#     interrupted context and the PROT_NONE load would fault again.
#   - the abandoned-frame line can only come from this arm: nothing else in
#     the boot abandons a trap-delivered frame, and it must sit between this
#     arm's E7AB-LEAP and its E7AB-OK.
#   - the earlier fault arms must still pass on the SAME boot, which is what
#     keeps this a new capability rather than a change of behaviour —
#     including for a tkill-delivered handler, whose 14s still go to the
#     sidecar (its frame is out of this branch because fault_active is 0).
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

[ -s user/sidecar/fixtures/e7_fault_abandon_probe.flat ] \
    && sok "the abandoning arm's flat payload is present" \
    || sbad "user/sidecar/fixtures/e7_fault_abandon_probe.flat is missing/empty"
grep -qF 'E7AB-ARM' user/sidecar/fixtures/e7_fault_abandon_probe.S \
    && sok "the abandoning arm carries its set-up marker" \
    || sbad "the fixture lost E7AB-ARM"
grep -qF 'E7AB-LEAP' user/sidecar/fixtures/e7_fault_abandon_probe.S \
    && sok "the handler names the jump it leaves by (E7AB-LEAP)" \
    || sbad "the fixture lost its jump marker"
grep -qF 'mov     0(%r12), %rsp' user/sidecar/fixtures/e7_fault_abandon_probe.S \
    && sok "the handler pops rsp back to the setjmp caller, above the frame" \
    || sbad "the fixture no longer leaves the frame by a longjmp-style rsp pop"
grep -qF 'movq    $0x4000, 0(%rsp)' user/sidecar/fixtures/e7_fault_abandon_probe.S \
    && sok "the arm blocks SIGTERM before the fault: the mask the frame RECORDS" \
    || sbad "the fixture no longer blocks a signal the frame can record"
grep -qF 'movq    $0x200, 0(%rsp)' user/sidecar/fixtures/e7_fault_abandon_probe.S \
    && sok "the handler moves the mask inside the window (SIGUSR1) — the edit the closure drops" \
    || sbad "the fixture no longer makes the in-window mask edit"
grep -qF 'cmp     $0x4000, %rax' user/sidecar/fixtures/e7_fault_abandon_probe.S \
    && sok "the query after the jump demands the frame's recorded 0x4000" \
    || sbad "the fixture no longer checks the mask the frame recorded"
grep -qF 'pub fn spawn_fault_abandon<' user/sidecar/src/e7_thread_death.rs \
    && sok "the sidecar carries the abandoning arm" \
    || sbad "e7_thread_death.rs no longer defines spawn_fault_abandon"
grep -qF 'crate::e7_thread_death::spawn_fault_abandon' user/sidecar/src/entry.rs \
    && sok "the sidecar spawns it through the real create" \
    || sbad "entry.rs no longer spawns the abandoning arm"
grep -qF 'if (cur->linux_sig_fault_active &&' kernel/syscall_dispatch.c \
    && sok "the closure is gated on the TRAP's frame (a tkill frame is not one)" \
    || sbad "the dispatch lost the abandoned-frame gate"
grep -qF 'fault handler abandoned the frame at ' kernel/syscall_dispatch.c \
    && sok "the kernel names the abandonment on the serial log" \
    || sbad "the kernel's abandoned-frame line is gone"
grep -qF 'mask window closed at %lx%s' kernel/syscall_dispatch.c \
    && sok "…and names the mask both ends agree on when it closes" \
    || sbad "the log line no longer reports the closed mask"

[ "$src_fails" -eq 0 ] || { echo "linux_fault_abandon_check: $src_fails source failure(s)"; exit 1; }

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
    if grep -aqF 'E7AB-ARM' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
if [ "$saw" -eq 0 ]; then
    echo "FAIL: the abandoning arm never reached its set-up marker in 480s"
    qemu_err
    tail -40 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7AB_LOG:-/tmp/linux_fault_abandon_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7AB_LOG:-/tmp/linux_fault_abandon_boot.log}"
    exit 1
fi
sleep 10

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

grep -aqF 'E7AB-FAIL' "$LOG" && bad "the abandoning arm refused its own set-up (E7AB-FAIL)" \
    || ok "the abandoning arm's set-up was accepted"
grep -aqF 'E7AB-LEAP' "$LOG" \
    && ok "the fault handler ran and left by its jump (E7AB-LEAP)" \
    || bad "no E7AB-LEAP — the handler never reached the longjmp"
grep -aqF 'E7AB-MASK' "$LOG" \
    && bad "the query after the jump read the dead handler's mask (E7AB-MASK): the window was still the kernel's" \
    || ok "the query after the jump did not read the dead handler's mask (no E7AB-MASK)"
grep -aqF 'E7AB-RET' "$LOG" \
    && bad "the 15 after the jump restored a frame that should have been dropped (E7AB-RET)" \
    || ok "the 15 after the jump did not restore a frame (no E7AB-RET)"
grep -aqF 'E7AB-LOOP' "$LOG" \
    && bad "the faulting load ran again (E7AB-LOOP): the frame was still installed" \
    || ok "the faulting load never ran twice (no E7AB-LOOP)"
grep -aqF 'E7AB-BAD' "$LOG" \
    && bad "the handler was entered for something other than this arm's #PF (E7AB-BAD)" \
    || ok "the handler saw this arm's #PF (no E7AB-BAD)"
grep -aqF 'E7AB-OK' "$LOG" \
    && ok "the arm's own verdict: mask 0x4000 and 15 = -EINVAL after the jump (E7AB-OK)" \
    || bad "no E7AB-OK — the mask or the frame state after the jump was wrong"

# The positive: the kernel logged the abandonment, named the frame's recorded
# mask, and the whole trio is ordered inside this arm's window.
grep -aqF '[E7] fault handler abandoned the frame at ' "$LOG" \
    && ok "the kernel logged the abandoned frame ('[E7] fault handler abandoned the frame at …')" \
    || bad "no '[E7] fault handler abandoned the frame' line — the closure never fired"
grep -aqF ': mask window closed at 4000' "$LOG" \
    && ok "…and closed it at the mask the frame recorded (0x4000)" \
    || bad "the abandoned-frame line did not report 0x4000 — the closure restored something the sidecar cannot agree with"
if awk '/E7AB-LEAP/ { if (!l) l = NR }
        /\[E7\] fault handler abandoned the frame at / { if (l && !a) a = NR }
        /E7AB-OK/ { if (a && !o) o = NR }
        END { exit !(l && a && o && l < a && a < o) }' "$LOG" 2>/dev/null; then
    ok "the abandonment sits between this arm's jump and its verdict (LEAP < abandoned < OK)"
else
    bad "the abandoned-frame line is absent or outside this arm's window (LEAP < abandoned < OK)"
fi
grep -aqF '[E7] fault sig 11 ' "$LOG" \
    && ok "the #PF was delivered to a handler (not a kill)" \
    || bad "no '[E7] fault sig 11 …' line"
grep -aqF '[E7] fault-abandon: spaw' "$LOG" \
    && ok "the arm was spawned through the sidecar's real create path" \
    || bad "no '[E7] fault-abandon: spaw…' line"

# …and the arms that must not change still pass on this same boot.
grep -aqF 'E7FR-OK' "$LOG" \
    && ok "increment 32's returning handler still resumes its faulting load" \
    || bad "E7FR-OK is absent: the fault handler's return regressed"
grep -aqF 'E7FH-OK' "$LOG" \
    && ok "increment 31's handler still runs and writes E7FH-OK" \
    || bad "E7FH-OK is absent: the increment-31 arm regressed"

echo ""
if [ "$fails" -eq 0 ]; then
    echo "linux_fault_abandon_check: 0 failure(s)"
    exit 0
fi
echo "linux_fault_abandon_check: $fails failure(s)"
cp "$LOG" "${E7AB_LOG:-/tmp/linux_fault_abandon_boot.log}" 2>/dev/null && \
    echo "boot log kept at ${E7AB_LOG:-/tmp/linux_fault_abandon_boot.log}"
exit 1
