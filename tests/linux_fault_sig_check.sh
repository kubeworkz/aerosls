#!/usr/bin/env bash
# linux_fault_sig_check.sh — E7 increment 31 (P1-C): the fault's own delivery,
# end to end on a boot.
#
# The host test (tests/linux_fault_sig_host_test.c) proves the DECISION: a
# handler plus a restorer is a frame, SIG_DFL/SIG_IGN/a blocked signal is a
# kill, si_addr is CR2, and the interrupted registers are what the frame
# records. This script proves the WIRING on the real machine, and it proves
# both answers in ONE boot so the difference between them is the handler and
# nothing else:
#
#   e7.tdeath.fault.0    the same PROT_NONE read with NO handler installed
#                        -> "[FAULT] Ring-3 #PF ... killing process"
#   e7.fault.handler.0   the same PROT_NONE read with SIGSEGV installed
#                        -> "[E7] fault sig 11 (code N) addr 0x… -> handler …"
#                        -> "E7FH-OK", written by the handler only when
#                           si_signo == 11, si_code is MAPERR/ACCERR and
#                           si_addr EQUALS the page the program mapped
#
# The last line is the property no log line can fake: si_addr is checked
# against a register the frame restored (r13), so a frame that recorded the
# handler's own context, or a si_addr invented from RIP, writes E7FH-BAD.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

for p in e7_fault_handler_probe e7_thread_fault_probe; do
    [ -s "user/sidecar/fixtures/$p.flat" ] \
        && sok "the $p flat payload is present" \
        || sbad "user/sidecar/fixtures/$p.flat is missing/empty"
done
grep -qF 'E7FH-ARM' user/sidecar/fixtures/e7_fault_handler_probe.S \
    && sok "the handler arm carries its set-up marker" \
    || sbad "the handler fixture lost E7FH-ARM"
grep -qF 'E7FH-OK' user/sidecar/fixtures/e7_fault_handler_probe.S \
    && sok "the handler writes its own success marker" \
    || sbad "the handler fixture lost E7FH-OK"
grep -qF 'SA_SIGINFO' user/sidecar/fixtures/e7_fault_handler_probe.S \
    && sok "the arm installs SA_SIGINFO (so si_addr is reachable)" \
    || sbad "the handler fixture no longer asks for siginfo"
grep -qF 'cmp     %r13, %rax' user/sidecar/fixtures/e7_fault_handler_probe.S \
    && sok "the handler compares si_addr against the page it mapped" \
    || sbad "the handler no longer proves si_addr"
grep -qF 'pub mod e7_thread_death;' user/sidecar/src/lib.rs \
    && grep -qF 'crate::e7_thread_death::spawn_fault_handler' user/sidecar/src/entry.rs \
    && sok "the sidecar spawns the handler arm through the real create" \
    || sbad "entry.rs no longer spawns the fault-handler arm"
grep -qF 'fcur->linux_cr2 = faulting_address;' kernel/stubs.c \
    && sok "the #PF captures CR2 into the descriptor (the real site)" \
    || sbad "kernel/stubs.c no longer records linux_cr2 at the fault"
grep -qF 'linux_fault_sig_plan(&fcur->linux_fault_act[fi],' kernel/stubs.c \
    && sok "the fault path asks the delivery plan before it kills" \
    || sbad "kernel/stubs.c no longer plans a delivery"

[ "$src_fails" -eq 0 ] || { echo "linux_fault_sig_check: $src_fails source failure(s)"; exit 1; }

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
for _ in $(seq 1 420); do
    if grep -aqF 'E7FH-ARM' "$LOG" 2>/dev/null &&
       grep -aqF 'E7TD-FAULT-ARM' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
if [ "$saw" -eq 0 ]; then
    echo "FAIL: the two fault arms did not both reach their set-up markers in 420s"
    qemu_err
    tail -40 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7FS_LOG:-/tmp/linux_fault_sig_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7FS_LOG:-/tmp/linux_fault_sig_boot.log}"
    exit 1
fi
# The handler runs (and exits) within a second of its marker.
sleep 8

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

grep -aqF 'E7FH-FAIL' "$LOG" && bad "the handler arm refused its own set-up (E7FH-FAIL)" \
    || ok "the handler arm's set-up was accepted"
grep -aqF 'E7FH-BAD' "$LOG" && bad "the handler saw the wrong signo/si_code/si_addr (E7FH-BAD)" \
    || ok "the handler matched signo, si_code and the faulting page"

fline="$(grep -aE '\[E7\] fault sig 11 \(code [12]\) addr 0x[0-9a-f]+ -> handler [0-9a-f]+, frame [0-9a-f]+' "$LOG" | head -1)"
if [ -n "$fline" ]; then
    ok "the kernel delivered a frame for the fault instead of killing: $fline"
    faddr="$(printf '%s\n' "$fline" | sed -n 's/.*addr 0x\([0-9a-f]*\) ->.*/\1/p')"
    if [ "$(( 0x$faddr % 4096 ))" -eq 0 ] && [ "$faddr" != "0" ]; then
        ok "si_addr is the page-aligned faulting address (0x$faddr), not 0 or an instruction"
    else
        bad "the delivered si_addr (0x$faddr) is not a page-aligned faulting address"
    fi
else
    bad "no '[E7] fault sig 11 … -> handler …' line — the fault path still killed a thread that had a handler"
fi

grep -aqF 'E7FH-OK' "$LOG" \
    && ok "the handler ran in ring 3 and wrote E7FH-OK" \
    || bad "E7FH-OK is absent: the handler never ran, or si_addr did not match the page"

# The OTHER arm, on the same boot: no handler installed means the attributed
# kill still happens. One boot, two answers, one difference.
grep -aqE '\[FAULT\] Ring-3 #PF .*addr=0x[0-9a-f]+.*killing process' "$LOG" \
    && ok "the handlerless arm still died by the attributed Ring-3 #PF kill" \
    || bad "the handlerless arm no longer shows the attributed kill — a fault without a handler must still be one"

# The klog helper writes its label and its number as separate chunks, so the
# line is read by its PREFIX (the two death arms' lines interleave the same
# way); the create path itself is already proven by the arm's own markers.
grep -aqF '[E7] fault-handler: spaw' "$LOG" \
    && ok "the arm was spawned through the sidecar's real create path" \
    || bad "no '[E7] fault-handler: spaw…' line"

echo ""
if [ "$fails" -eq 0 ]; then
    echo "linux_fault_sig_check: 0 failure(s)"
    exit 0
fi
echo "linux_fault_sig_check: $fails failure(s)"
cp "$LOG" "${E7FS_LOG:-/tmp/linux_fault_sig_boot.log}" 2>/dev/null && \
    echo "boot log kept at ${E7FS_LOG:-/tmp/linux_fault_sig_boot.log}"
exit 1
