#!/usr/bin/env bash
# linux_openat_probe_check.sh — E7 TG-1 I4's native openat live probe.
#
# The source half of I4 (the 257 ABI row, the args[1] path plan, the lease
# state machine) is pinned by the host tests. This is the LIVE half: boot the
# ordinary ISO, wait for the sidecar-spawned `e7.openat.probe.0` child, and
# assert that its CLONED SIBLING — never the leader — forwarded openat(257,
# AT_FDCWD, "/", O_RDONLY|O_DIRECTORY) and got a real fd, that the same
# sibling's close(3) returned 0, that the kernel logged the lease acquired for
# syscall 257 by the child's TID, that the child exited thread-locally and
# that exit_group tore the group down exactly once.
#
# Markers on serial (fixtures/e7_openat_probe.S):
#   E7OA-START      the program ran
#   E7OA-OPEN-OK    the sibling's openat returned fd >= 3
#   E7OA-CLOSE-OK   the sibling's close of that fd returned 0
#   E7OA-OK         the exchange completed
#   E7OA-FAIL       any refusal/mismatch (must be absent)
# Kernel lines:
#   [E7] clone: TID <ctid> joins group <gid> (leader <gid>)
#   [E7] forward lease: TID <ctid> group <gid> acquired for syscall 257
#   [E7] thread exit: TID <ctid> group <gid> code=0
#   [E7] group exit: group <gid> (leader <gid>) torn down once   (exactly 1)
#
# The probe's group is identified by the clone line whose group also owns a
# `forward lease ... acquired for syscall 257` line for the cloned TID; the
# other probes never forward openat, so the live groups cannot be confused.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

# ─── Source half (runs even where the ISO/QEMU is unavailable) ───────────
src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

[ -s user/sidecar/fixtures/e7_openat_probe.flat ] \
    && sok "the openat probe's flat payload is present" \
    || sbad "user/sidecar/fixtures/e7_openat_probe.flat is missing/empty"
grep -qF 'E7OA-OPEN-OK' user/sidecar/fixtures/e7_openat_probe.S 2>/dev/null \
    && sok "the fixture carries the open marker" \
    || sbad "the fixture no longer carries E7OA-OPEN-OK"
grep -qF 'pub mod e7_openat_probe;' user/sidecar/src/lib.rs \
    && sok "the sidecar declares the openat-probe module" \
    || sbad "user/sidecar/src/lib.rs does not declare e7_openat_probe"
grep -qF 'crate::e7_openat_probe::spawn' user/sidecar/src/entry.rs \
    && sok "the sidecar boot spawns the openat probe through the real create" \
    || sbad "entry.rs no longer spawns e7_openat_probe"
grep -qE '\{ *257, *LSC_FD,' kernel/linux_abi.c \
    && sok "openat (257) has a real ABI table row" \
    || sbad "kernel/linux_abi.c has no { 257, LSC_FD } row"
grep -qF 'LINUX_OPENAT' kernel/linux_abi.c \
    && sok "the path plan knows openat" \
    || sbad "kernel/linux_abi.c no longer names LINUX_OPENAT"
grep -qF 'linux_group_fwd_acquire' kernel/syscall_dispatch.c \
    && sok "the dispatcher takes the forward lease before posting" \
    || sbad "syscall_dispatch.c no longer calls linux_group_fwd_acquire"
grep -qF 'linux_forward_lease_release' kernel/syscall_dispatch.c \
    && sok "the dispatcher releases the lease at the reply" \
    || sbad "syscall_dispatch.c no longer calls linux_forward_lease_release"
grep -qF 'LINUX_OPENAT' user/sidecar/src/linux.rs \
    && sok "the sidecar serves openat" \
    || sbad "user/sidecar/src/linux.rs no longer serves openat"
grep -qF 'proc_group_cap_owner' kernel/chan.c \
    && sok "channel slot resolution is group-aware for cloned siblings" \
    || sbad "kernel/chan.c no longer resolves a member to its group's cap table"
grep -qF 'per_cpu_data[0].kernel_rsp' kernel/process.c \
    && sok "process_find_current uses the per-thread syscall stack" \
    || sbad "kernel/process.c lost the per-thread current match"
[ -s tests/linux_thread_group_host_test.c ] \
    && sok "the lease host coverage is present (the group host test)" \
    || sbad "no host test covers the forward lease"

[ "$src_fails" -eq 0 ] || { echo "linux_openat_probe_check: $src_fails source failure(s)"; exit 1; }

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
for _ in $(seq 1 300); do
    if grep -aqF 'E7OA-OK' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done

# This probe's group: the clone line whose group owns a lease line for 257
# naming the cloned TID.
gid=""
ctid=""
while IFS= read -r line; do
    g="$(printf '%s\n' "$line" | sed -n 's/.*TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\2/p')"
    t="$(printf '%s\n' "$line" | sed -n 's/.*TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\1/p')"
    [ -n "$g" ] || continue
    if grep -aqF "[E7] forward lease: TID $t group $g acquired for syscall 257" "$LOG" 2>/dev/null; then
        gid="$g"
        ctid="$t"
        break
    fi
done < <(grep -aF '[E7] clone: TID ' "$LOG" 2>/dev/null)

if [ -n "$gid" ]; then
    for _ in $(seq 1 120); do
        if grep -aqF "[E7] group exit: group $gid " "$LOG" 2>/dev/null; then break; fi
        if ! kill -0 "$QPID" 2>/dev/null; then break; fi
        sleep 1
    done
fi

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

[ "$saw" -eq 1 ] || {
    echo "FAIL: the openat probe did not reach E7OA-OK in 300s"
    qemu_err
    tail -50 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7OA_LOG:-/tmp/linux_openat_probe_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7OA_LOG:-/tmp/linux_openat_probe_boot.log}"
    exit 1
}

grep -aqF 'E7OA-START' "$LOG" \
    && ok "the openat probe printed its start marker" \
    || bad "E7OA-START is missing"
grep -aqF 'E7OA-OPEN-OK' "$LOG" \
    && ok "the sibling's openat returned a real fd" \
    || bad "E7OA-OPEN-OK is missing"
grep -aqF 'E7OA-CLOSE-OK' "$LOG" \
    && ok "the sibling's close of that fd returned 0" \
    || bad "E7OA-CLOSE-OK is missing"
grep -aqF 'E7OA-OK' "$LOG" \
    && ok "the probe completed its whole exchange" \
    || bad "E7OA-OK is missing"
if grep -aqF 'E7OA-FAIL' "$LOG"; then
    bad "the probe reported E7OA-FAIL"
else
    ok "no E7OA-FAIL marker"
fi

if [ -n "$gid" ]; then
    ok "the openat probe's group is $gid (clone TID $ctid)"
    grep -aqF "[E7] forward lease: TID $ctid group $gid acquired for syscall 257" "$LOG" \
        && ok "the CLONED sibling (TID $ctid) held the lease for openat" \
        || bad "no lease-acquire line for the child's openat"
    acquires="$(grep -acF "[E7] forward lease: TID $ctid group $gid acquired for " "$LOG")"
    [ "${acquires:-0}" -ge 2 ] \
        && ok "the sibling forwarded twice (openat and close), $acquires lease lines" \
        || bad "expected at least 2 lease-acquire lines for TID $ctid, saw ${acquires:-0}"
    if grep -aqF "[E7] forward lease: TID $ctid group $gid waits " "$LOG"; then
        ok "a forward had to wait for the lease (busy path exercised)"
    fi
    grep -aqF "[E7] thread exit: TID $ctid " "$LOG" \
        && ok "exit(60) logged the child TID $ctid as a thread-local exit" \
        || bad "no thread-exit line for the cloned TID $ctid"
    group_exits="$(grep -acF "[E7] group exit: group $gid " "$LOG")"
    [ "$group_exits" -eq 1 ] \
        && ok "the group was torn down exactly once (one group-exit line)" \
        || bad "expected exactly one group-exit line for group $gid, saw $group_exits"
else
    bad "no openat-probe clone line with a syscall-257 lease (the sibling never forwarded)"
fi

echo ""
echo "linux_openat_probe_check: $fails failure(s)"
if [ "$fails" -ne 0 ]; then
    cp "$LOG" "${E7OA_LOG:-/tmp/linux_openat_probe_boot.log}" 2>/dev/null && \
        echo "boot log kept at ${E7OA_LOG:-/tmp/linux_openat_probe_boot.log}"
fi
[ "$fails" -eq 0 ]
