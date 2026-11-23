#!/usr/bin/env bash
# linux_thread_probe_check.sh — E7 TG-1 I2's native two-thread live probe.
#
# The source half of I2 (clone flags/context, group admission/leave,
# gettid/getpid/exit classes) is pinned by the host tests and the source
# guards. This is the LIVE half: boot the ordinary ISO, wait for the
# sidecar-spawned `e7.thread.probe.0` child, and assert that one Linux
# process really created a sibling in its own address space, that both
# threads saw the group id, that the child's write was visible to the
# parent, that `exit` (60) released only the child, and that `exit_group`
# (231) tore the group down exactly once.
#
# Markers on serial (fixtures/e7_thread_probe.S):
#   E7TG-START       the program ran
#   E7TG-CHILD-OK    the parent saw the child's tid and shared write
#   E7TG-SHARED-OK   the parent's own write after the settle window
#   E7TG-FAIL        any refusal/mismatch (must be absent)
# Kernel lines:
#   [E7] clone: TID <tid> joins group <gid> (leader <gid>)
#   [E7] thread exit: TID <tid> group <gid> code=0
#   [E7] group exit: group <gid> (leader <gid>) torn down once   (exactly 1)
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

# ─── Source half (runs even where the ISO/QEMU is unavailable) ───────────
src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

[ -s user/sidecar/fixtures/e7_thread_probe.flat ] \
    && sok "the probe's flat payload is present" \
    || sbad "user/sidecar/fixtures/e7_thread_probe.flat is missing/empty"
if grep -qF 'E7TG-SHARED-OK' user/sidecar/fixtures/e7_thread_probe.S 2>/dev/null; then
    sok "the fixture carries the shared-write marker"
else
    sbad "the fixture no longer carries E7TG-SHARED-OK"
fi
grep -qF 'pub mod e7_thread_probe;' user/sidecar/src/lib.rs \
    && sok "the sidecar declares the thread-probe module" \
    || sbad "user/sidecar/src/lib.rs does not declare e7_thread_probe"
grep -qF 'crate::e7_thread_probe::spawn' user/sidecar/src/entry.rs \
    && sok "the sidecar boot spawns the thread probe through the real create" \
    || sbad "entry.rs no longer spawns e7_thread_probe"

[ "$src_fails" -eq 0 ] || { echo "linux_thread_probe_check: $src_fails source failure(s)"; exit 1; }

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

# Wait for the probe's last positive marker (or a dead QEMU / timeout).
saw=0
for _ in $(seq 1 300); do
    if grep -aqF 'E7TG-SHARED-OK' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done

# The parent prints SHARED-OK and then asks for exit_group; the teardown line
# is the sidecar's reply to that request, so it lands after the marker. The
# boot runs several Linux groups, so scope the wait to THIS probe's group id
# (from the clone line) and wait for that line (or a dead QEMU / timeout).
clone_line="$(grep -aF '[E7] clone: TID ' "$LOG" 2>/dev/null | head -1)"
ctid="$(printf '%s\n' "$clone_line" | sed -n 's/.*clone: TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\1/p')"
gid="$(printf '%s\n' "$clone_line" | sed -n 's/.*clone: TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\2/p')"
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
    echo "FAIL: the thread probe did not reach E7TG-SHARED-OK in 300s"
    qemu_err
    tail -40 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7TP_LOG:-/tmp/linux_thread_probe_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7TP_LOG:-/tmp/linux_thread_probe_boot.log}"
    exit 1
}

grep -aqF 'E7TG-START' "$LOG" \
    && ok "the probe printed its start marker" \
    || bad "E7TG-START is missing"
grep -aqF 'E7TG-CHILD-OK' "$LOG" \
    && ok "the child thread saw its own tid, the group's tgid and wrote the shared word" \
    || bad "E7TG-CHILD-OK is missing"
grep -aqF 'E7TG-SHARED-OK' "$LOG" \
    && ok "the parent observed the child's shared-memory write" \
    || bad "E7TG-SHARED-OK is missing"
if grep -aqF 'E7TG-FAIL' "$LOG"; then
    bad "the probe reported E7TG-FAIL"
else
    ok "no E7TG-FAIL marker"
fi

clone_line="$(grep -a '\[E7\] clone: TID ' "$LOG" | head -1)"
if [ -n "$clone_line" ]; then
    ok "the kernel logged the clone: $clone_line"
    ctid="$(printf '%s\n' "$clone_line" | sed -n 's/.*TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\1/p')"
    cgid="$(printf '%s\n' "$clone_line" | sed -n 's/.*TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\2/p')"
    if [ -n "$ctid" ] && [ -n "$cgid" ] && [ "$ctid" != "$cgid" ]; then
        ok "the child TID ($ctid) is distinct from the group id ($cgid)"
    else
        bad "the clone line does not carry two distinct TIDs"
    fi
    grep -aq "\[E7\] thread exit: TID $ctid " "$LOG" \
        && ok "exit(60) logged the child TID $ctid as a thread-local exit" \
        || bad "no thread-exit line for the cloned TID $ctid"
else
    bad "no '[E7] clone: TID' line — clone never created a sibling"
fi

group_exits="$(grep -acF "[E7] group exit: group $gid " "$LOG")"
[ "$group_exits" -eq 1 ] \
    && ok "the group was torn down exactly once (one group-exit line)" \
    || bad "expected exactly one group-exit line, saw $group_exits"

echo ""
echo "linux_thread_probe_check: $fails failure(s)"
if [ "$fails" -ne 0 ]; then
    cp "$LOG" "${E7TP_LOG:-/tmp/linux_thread_probe_boot.log}" 2>/dev/null && \
        echo "boot log kept at ${E7TP_LOG:-/tmp/linux_thread_probe_boot.log}"
fi
[ "$fails" -eq 0 ]