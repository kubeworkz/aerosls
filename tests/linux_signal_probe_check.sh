#!/usr/bin/env bash
# linux_signal_probe_check.sh — E7 increment 28's LIVE half: a handler
# actually executes in ring 3.
#
# The static half (frame layout, request decoding, the park bridge, the
# install offsets, the wire's three ends) is pinned by
# tests/linux_sigdeliver_host_test.c and guard clauses P–P11. None of that
# can show that a task is ENTERED in a handler: that needs a real boot with
# a real ring-3 program.
#
# The program is the existing probe (fixtures/e7_linux_probe.S), whose last
# step installs SIGUSR1 -> .Lhandler and raises it with tkill, so this check
# adds no new fixture, no new spawn hook and no new archive variant — the
# boot that already runs the probe now also measures the delivery.
#
# Markers on serial (fixtures/e7_linux_probe.S):
#   [E7] sig 10 (code 0) -> handler 0x...     the kernel's own delivery line
#   SIGHAND-OK                                written by the HANDLER, i.e.
#                                             ring-3 code the kernel jumped to
#   SIGPROBE-EXIT 43                          placehold: the handler's exit
#                                             code (see below)
# Absent-on-success markers (the 391/392 discipline):
#   409   the tkill returned instead of delivering
#   410   rt_sigaction refused the install
#   411   a handler returned into the restorer (rt_sigreturn is not landed)
#
# What proves the handler ran, in order of strength:
#   1. SIGHAND-OK appears — only .Lhandler writes those bytes;
#   2. the [E7] sig 10 line names a handler address — only a delivery prints
#      it, and nothing else in the boot gives this task a handler;
#   3. the probe's own success markers (E7FD-WRITE-OK, E7MEM-*) all appear
#      FIRST, so the flow that reached the handler is the one that passed
#      every seam before it;
#   4. 409 is absent — had the syscall returned, the probe would have printed
#      it and never reached the handler.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

echo "── source half ────────────────────────────────────────────────"
FIX=user/sidecar/fixtures/e7_linux_probe.S
[ -s user/sidecar/fixtures/e7_linux_probe.flat ] \
    && ok "the probe's flat payload is present" \
    || bad "user/sidecar/fixtures/e7_linux_probe.flat is missing/empty"
grep -qF '.Lsig_try:' "$FIX" && grep -qF 'SIGHAND-OK' "$FIX" \
    && ok "the fixture carries the signal flow and its handler's marker" \
    || bad "the fixture lost the signal flow or SIGHAND-OK"
grep -qF 'mov     $200, %eax' "$FIX" && grep -qF 'mov     $13, %eax' "$FIX" \
    && ok "the flow is rt_sigaction(13) then tkill(200) — the wired pair" \
    || bad "the fixture no longer issues 13 and 200"
grep -qF '[E7] sig %u (code %u) -> handler %lx' kernel/process.c \
    && ok "the kernel still prints the delivery line this check reads" \
    || bad "kernel/process.c no longer prints the [E7] sig line"

[ -f sls_operating_system.iso ] || {
    echo "ABORT: sls_operating_system.iso not found — run 'make x86-iso' first." >&2
    exit 2
}

echo "── live half (boot) ───────────────────────────────────────────"
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
    if grep -aqF 'SIGHAND-OK' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
# Let the handler's exit_group land its teardown lines.
for _ in $(seq 1 30); do
    grep -aqF 'SIGHAND-OK' "$LOG" 2>/dev/null && break
    ! kill -0 "$QPID" 2>/dev/null && break
    sleep 1
done
sleep 2

if [ "$saw" -eq 1 ]; then
    ok "SIGHAND-OK reached the serial log — the bytes were written by the HANDLER"
else
    bad "no SIGHAND-OK: the handler never ran (see the log excerpt above)"
fi

if grep -aqF '[E7] sig 10 (code 0) -> handler ' "$LOG" 2>/dev/null; then
    ok "the kernel printed its own delivery line: [E7] sig 10 -> handler ..."
else
    bad "the kernel's [E7] sig 10 -> handler line is absent: no delivery was built"
fi

# %lx, no 0x prefix — the line as kernel_serial_printf writes it:
#   [E7] sig 10 (code 0) -> handler 400000000427, frame 400000004c98, mask 200
if grep -aqE '\[E7\] sig 10 \(code 0\) -> handler [0-9a-f]+, frame [0-9a-f]+, mask [0-9a-f]+' "$LOG" 2>/dev/null; then
    ok "the line carries the handler's address, the frame's and the handler's mask — a real build, not a bare render"
else
    bad "the delivery line is malformed (no handler/frame/mask)"
fi

# 0x200 is bit (10-1): the signal itself, blocked for its own handler —
# the mask the sidecar computed and the kernel installed must agree.
if grep -aqE '\[E7\] sig 10 \(code 0\) -> handler [0-9a-f]+, frame [0-9a-f]+, mask 200$' "$LOG" 2>/dev/null; then
    ok "the mask on the delivery line is 0x200 — SIGUSR1's own bit, which is the number both ends computed"
else
    bad "the handler's mask is not 0x200: the two ends disagree about the mask the handler runs under"
fi

if grep -aqF 'E7FD-WRITE-OK' "$LOG" && grep -aqF 'E7MEM-SOFT-OK' "$LOG"; then
    ok "the probe's earlier seams all passed in the SAME run (write + soft mprotect)"
else
    bad "the probe did not reach its earlier markers, so this boot proves less than claimed"
fi

for marker in 409 410 411; do
    if grep -aqE "(^|[^0-9])$marker([^0-9]|$)" "$LOG" 2>/dev/null \
       && grep -aqF "num=$marker" "$LOG" 2>/dev/null; then
        bad "MARKER $marker appeared: $( [ "$marker" = 409 ] && echo 'the tkill returned instead of delivering' || { [ "$marker" = 410 ] && echo 'rt_sigaction refused the install' || echo 'a handler returned into the restorer'; } )"
    else
        ok "MARKER $marker absent, as success requires"
    fi
done

# Keep the boot transcript for diagnosis either way: a runtime check that
# deletes its own evidence can only report a verdict, and the next reader
# needs the serial lines around the failure.
cp -f "$LOG" /tmp/e7_signal_probe_boot.log 2>/dev/null || true
echo "      (serial transcript: /tmp/e7_signal_probe_boot.log)"

echo
if [ "$fails" -eq 0 ]; then
    echo "linux_signal_probe_check: all clauses hold — the handler ran in ring 3"
    exit 0
fi
echo "linux_signal_probe_check: $fails failed"
exit 1
