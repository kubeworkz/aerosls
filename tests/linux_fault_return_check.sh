#!/usr/bin/env bash
# linux_fault_return_check.sh — E7 increment 32 (P1-C): a fault-delivered
# handler's RETURN, end to end on a boot.
#
# The host test (tests/linux_fault_return_host_test.c) proves the two
# decisions: WHICH return the kernel may take over (an installed frame that
# came from the trap, in answer to exactly the sidecar's -EINVAL, never a
# consent) and WHAT it restores (the trap's own frame, decoded by the same
# rt_sigreturn decoder — including a greg the handler rewrote through its
# ucontext pointer). This script proves the WIRING on the real machine:
#
#   e7.fault.return.0   a PROT_NONE read with SIGSEGV installed. The handler
#                       checks si_addr, rewrites the interrupted r13 in its
#                       ucontext to a page holding 0xfeedface, and RETURNS
#                       through its restorer's rt_sigreturn(15).
#                       -> "[E7] rt_sigreturn: restored rip … rsp … rax … "
#                       -> the faulting load runs AGAIN, at the new r13, and
#                          reads 0xfeedface
#                       -> "E7FR-OK", written only after that comparison
#
# Why the markers are a proof and not a claim:
#   - E7FR-BAD is written if the resumed context is wrong: the load faults a
#     second time, the r13 comparison fails, or the restorer's 15 came back
#     with an errno (the kernel did not restore — it returned the refusal).
#   - E7FR-LOOP is written by the handler itself when it is entered TWICE.
#     That is the signature of a return that did not take effect, so a
#     missing restore is a named line in the log, never a silent hang.
#   - E7FR-OK requires %eax == 0xfeedface AND r13 == the pointer the handler
#     wrote, so a resume at the wrong rip, or one that restored the
#     pre-handler registers, cannot produce it.
#
# The earlier arms must still pass on the SAME boot (increment 31's handler
# that exits, and the handlerless kill), which is what keeps this a new
# capability rather than a change of behaviour.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

[ -s user/sidecar/fixtures/e7_fault_return_probe.flat ] \
    && sok "the returning arm's flat payload is present" \
    || sbad "user/sidecar/fixtures/e7_fault_return_probe.flat is missing/empty"
grep -qF 'E7FR-ARM' user/sidecar/fixtures/e7_fault_return_probe.S \
    && sok "the returning arm carries its set-up marker" \
    || sbad "the fixture lost E7FR-ARM"
grep -qF 'E7FR-OK' user/sidecar/fixtures/e7_fault_return_probe.S \
    && sok "the RESUMED program writes its own success marker" \
    || sbad "the fixture lost E7FR-OK"
grep -qF 'mov     %rax, 80(%rdx)' user/sidecar/fixtures/e7_fault_return_probe.S \
    && sok "the handler rewrites the interrupted r13 through its ucontext" \
    || sbad "the handler no longer fixes a greg in the frame"
grep -qE '^    ret .*Lrestorer' user/sidecar/fixtures/e7_fault_return_probe.S \
    && sok "the handler RETURNS through the frame's pretcode (the restorer's 15)" \
    || sbad "the handler no longer returns through its restorer"
grep -qF 'E7FR-LOOP' user/sidecar/fixtures/e7_fault_return_probe.S \
    && sok "the handler names a second entry (a return that did not take effect)" \
    || sbad "the fixture lost its double-delivery detector"
grep -qF 'crate::e7_thread_death::spawn_fault_return' user/sidecar/src/entry.rs \
    && sok "the sidecar spawns the returning arm through the real create" \
    || sbad "entry.rs no longer spawns the returning arm"
grep -qF 'int linux_sig_fault_return_ok(int sig_active, int fault_active, uint64_t rax,' kernel/linux_abi.h \
    && sok "the kernel names the one return it may take over" \
    || sbad "kernel/linux_abi.h no longer declares the fault return gate"
grep -qF 'fcur->linux_sig_fault_active = 1;' kernel/stubs.c \
    && sok "the #PF delivery marks its frame as the trap's" \
    || sbad "kernel/stubs.c no longer marks a fault delivery"
grep -qF 'linux_user_copy_out(frame, pd->cr3, pd->linux_sig_frame_rsp,' kernel/process.c \
    && sok "the return reads the frame at the address the delivery recorded" \
    || sbad "kernel/process.c no longer reads the recorded frame address"

[ "$src_fails" -eq 0 ] || { echo "linux_fault_return_check: $src_fails source failure(s)"; exit 1; }

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
    if grep -aqF 'E7FR-ARM' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
if [ "$saw" -eq 0 ]; then
    echo "FAIL: the returning arm never reached its set-up marker in 480s"
    qemu_err
    tail -40 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7FR_LOG:-/tmp/linux_fault_return_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7FR_LOG:-/tmp/linux_fault_return_boot.log}"
    exit 1
fi
# The handler runs, returns and the resumed program exits within seconds.
sleep 10

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

grep -aqF 'E7FR-FAIL' "$LOG" && bad "the returning arm refused its own set-up (E7FR-FAIL)" \
    || ok "the returning arm's set-up was accepted"
grep -aqF 'E7FR-BAD' "$LOG" && bad "the resumed context was wrong (E7FR-BAD) — a second fault, a bad r13, or the restorer's 15 came back with an errno" \
    || ok "the resumed context was accepted (no E7FR-BAD)"
grep -aqF 'E7FR-LOOP' "$LOG" && bad "the handler was entered twice (E7FR-LOOP): the first rt_sigreturn restored nothing" \
    || ok "the handler was entered exactly once (no E7FR-LOOP)"

grep -aqF 'E7FR-OK' "$LOG" \
    && ok "the faulting load ran AGAIN after the return and read the page the handler supplied (E7FR-OK)" \
    || bad "E7FR-OK is absent: the return did not resume the interrupted instruction with the handler's fix"

# The kernel's own restore line, and it must sit between this arm's set-up and
# its success marker — the ordering is what makes it THIS arm's return rather
# than the tkill probe's.
# The FIRST restore line after this arm's marker, and the first success
# marker after that — the boot also carries the tkill probe's return later
# on, and a `restored rip` line is not this arm's just because it exists.
if awk '/E7FR-ARM/ { a = NR }
        /\[E7\] rt_sigreturn: restored rip / { if (a && !r) r = NR }
        /E7FR-OK/ { if (a && r && !o) o = NR }
        END { exit !(a && r && o && a < r && r < o) }' "$LOG" 2>/dev/null; then
    ok "the kernel restored the interrupted context between the arm's set-up and its success ([E7] rt_sigreturn: restored rip …)"
else
    bad "no [E7] rt_sigreturn: restored rip line between E7FR-ARM and E7FR-OK — the return never reached linux_sig_return_apply"
fi

# The delivery itself: a fault line, and the arm was spawned by the sidecar.
grep -aqE '\[E7\] fault sig 11 \(code [12]\) addr 0x[0-9a-f]+ -> handler [0-9a-f]+, frame [0-9a-f]+' "$LOG" \
    && ok "the #PF was delivered to a handler (not a kill)" \
    || bad "no '[E7] fault sig 11 … -> handler …' line"
grep -aqF '[E7] fault-return: spaw' "$LOG" \
    && ok "the arm was spawned through the sidecar's real create path" \
    || bad "no '[E7] fault-return: spaw…' line"

# …and the earlier arms still pass on this same boot: increment 31's handler
# (which exits rather than returning) and the handlerless attributed kill.
grep -aqF 'E7FH-OK' "$LOG" \
    && ok "increment 31's handler still runs and writes E7FH-OK" \
    || bad "E7FH-OK is absent: the increment-31 arm regressed"
grep -aqE '\[FAULT\] Ring-3 #PF .*addr=0x[0-9a-f]+.*killing process' "$LOG" \
    && ok "the handlerless fault still dies by the attributed kill" \
    || bad "the handlerless arm no longer shows the attributed kill"

echo ""
if [ "$fails" -eq 0 ]; then
    echo "linux_fault_return_check: 0 failure(s)"
    exit 0
fi
echo "linux_fault_return_check: $fails failure(s)"
cp "$LOG" "${E7FR_LOG:-/tmp/linux_fault_return_boot.log}" 2>/dev/null && \
    echo "boot log kept at ${E7FR_LOG:-/tmp/linux_fault_return_boot.log}"
exit 1
