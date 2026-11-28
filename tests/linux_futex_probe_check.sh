#!/usr/bin/env bash
# linux_futex_probe_check.sh — E7 TG-1 I3's native futex live probe.
#
# The source half of I3 (op allowlist, deadline conversion, group/address
# keying, expiry) is pinned by tests/linux_futex_host_test.c. This is the LIVE
# half: boot the ordinary ISO, wait for the sidecar-spawned `e7.futex.probe.0`
# child, and assert that a parent really PARKED in FUTEX_WAIT_PRIVATE, that its
# cloned sibling's FUTEX_WAKE_PRIVATE returned 1 and resumed it with 0, that a
# 20 ms relative timeout returned ETIMEDOUT through the timer tick, that the
# child's exit(60) was thread-local, and that exit_group tore the group down
# exactly once.
#
# Markers on serial (fixtures/e7_futex_probe.S):
#   E7FX-START        the program ran
#   E7FX-WAIT-OK      the wait returned 0 and the child published wake=1
#   E7FX-TIMEOUT-OK   the relative timeout returned -ETIMEDOUT
#   E7FX-OK           the exchange completed
#   E7FX-FAIL         any refusal/mismatch (must be absent)
# Kernel lines:
#   [E7] clone: TID <tid> joins group <gid> (leader <gid>)
#   [E7] futex wait: TID <gid> group <gid> uaddr=0x... deadline=0 parked
#   [E7] futex wake: group <gid> uaddr=0x... woken=1
#   [E7] futex timeout: TID <gid> resumed with -ETIMEDOUT
#   [E7] thread exit: TID <ctid> group <gid> code=0
#   [E7] group exit: group <gid> (leader <gid>) torn down once   (exactly 1)
#
# The probe's group is identified by the clone line whose group also owns a
# futex-wait line: the I2 thread probe clones into a group with no futex use,
# so the two live probes cannot be confused even though both print clone lines.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

# ─── Source half (runs even where the ISO/QEMU is unavailable) ───────────
src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

[ -s user/sidecar/fixtures/e7_futex_probe.flat ] \
    && sok "the futex probe's flat payload is present" \
    || sbad "user/sidecar/fixtures/e7_futex_probe.flat is missing/empty"
if grep -qF 'E7FX-TIMEOUT-OK' user/sidecar/fixtures/e7_futex_probe.S 2>/dev/null; then
    sok "the fixture carries the timeout marker"
else
    sbad "the fixture no longer carries E7FX-TIMEOUT-OK"
fi
grep -qF 'pub mod e7_futex_probe;' user/sidecar/src/lib.rs \
    && sok "the sidecar declares the futex-probe module" \
    || sbad "user/sidecar/src/lib.rs does not declare e7_futex_probe"
grep -qF 'crate::e7_futex_probe::spawn' user/sidecar/src/entry.rs \
    && sok "the sidecar boot spawns the futex probe through the real create" \
    || sbad "entry.rs no longer spawns e7_futex_probe"
grep -qF 'LSC_FUTEX' kernel/linux_abi.c \
    && sok "futex (202) has a real ABI table row" \
    || sbad "kernel/linux_abi.c no longer classifies futex"
grep -qF 'linux_futex_block' kernel/syscall_dispatch.c \
    && sok "the dispatcher routes futex to the kernel-local block path" \
    || sbad "syscall_dispatch.c no longer calls linux_futex_block"
grep -qF 'linux_futex_deadline_tick' kernel/timer.c \
    && sok "the timer ISR expires futex waiters" \
    || sbad "timer.c no longer calls linux_futex_deadline_tick"
[ -s tests/linux_futex_host_test.c ] \
    && sok "the futex host test is present" \
    || sbad "tests/linux_futex_host_test.c is missing"

[ "$src_fails" -eq 0 ] || { echo "linux_futex_probe_check: $src_fails source failure(s)"; exit 1; }

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
    if grep -aqF 'E7FX-OK' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done

# The parent prints OK and then asks for exit_group; the teardown line is the
# sidecar's reply, so it lands after the marker. Identify THIS probe's group
# as the clone line whose group also has a futex-wait line.
gid=""
ctid=""
while IFS= read -r line; do
    g="$(printf '%s\n' "$line" | sed -n 's/.*TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\2/p')"
    t="$(printf '%s\n' "$line" | sed -n 's/.*TID \([0-9][0-9]*\) joins group \([0-9][0-9]*\).*/\1/p')"
    [ -n "$g" ] || continue
    if grep -aqF "[E7] futex wait: TID $g group $g " "$LOG" 2>/dev/null; then
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
    echo "FAIL: the futex probe did not reach E7FX-OK in 300s"
    qemu_err
    tail -50 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7FX_LOG:-/tmp/linux_futex_probe_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7FX_LOG:-/tmp/linux_futex_probe_boot.log}"
    exit 1
}

grep -aqF 'E7FX-START' "$LOG" \
    && ok "the futex probe printed its start marker" \
    || bad "E7FX-START is missing"
grep -aqF 'E7FX-WAIT-OK' "$LOG" \
    && ok "the parent's FUTEX_WAIT returned 0 after a real wake" \
    || bad "E7FX-WAIT-OK is missing"
grep -aqF 'E7FX-TIMEOUT-OK' "$LOG" \
    && ok "the 20 ms relative FUTEX_WAIT returned ETIMEDOUT" \
    || bad "E7FX-TIMEOUT-OK is missing"
grep -aqF 'E7FX-OK' "$LOG" \
    && ok "the probe completed its whole exchange" \
    || bad "E7FX-OK is missing"
if grep -aqF 'E7FX-FAIL' "$LOG"; then
    bad "the probe reported E7FX-FAIL"
else
    ok "no E7FX-FAIL marker"
fi

if [ -n "$gid" ]; then
    ok "the futex probe's group is $gid (clone TID $ctid)"
    grep -aqF "[E7] futex wait: TID $gid group $gid " "$LOG" \
        && ok "the parent parked under the group's key" \
        || bad "no parked futex wait for the parent's TID $gid"
    grep -aqF "[E7] futex wake: group $gid " "$LOG" \
        && ok "the sibling's wake ran against that group" \
        || bad "no futex wake line for group $gid"
    grep -aqF "[E7] futex wake: group $gid " "$LOG" && grep -aqF 'woken=1' "$LOG" \
        && ok "a wake resumed exactly one waiter" \
        || bad "no woken=1 futex wake line"
    grep -aqF "[E7] futex timeout: TID $gid " "$LOG" \
        && ok "the timed waiter was resumed with -ETIMEDOUT" \
        || bad "no futex timeout line for TID $gid"
    echo "      --- futex ts lines (diagnostic) ---"
    grep -a "futex ts:" "$LOG" 2>/dev/null | sed 's/^/      /' | head -20
    echo "      --- end ---"
    grep -aqF "[E7] thread exit: TID $ctid " "$LOG" \
        && ok "exit(60) logged the child TID $ctid as a thread-local exit" \
        || bad "no thread-exit line for the cloned TID $ctid"
    group_exits="$(grep -acF "[E7] group exit: group $gid " "$LOG")"
    [ "$group_exits" -eq 1 ] \
        && ok "the group was torn down exactly once (one group-exit line)" \
        || bad "expected exactly one group-exit line for group $gid, saw $group_exits"
else
    bad "no futex-probe clone line (clone never created the sibling)"
fi

echo ""
echo "linux_futex_probe_check: $fails failure(s)"
if [ "$fails" -ne 0 ]; then
    cp "$LOG" "${E7FX_LOG:-/tmp/linux_futex_probe_boot.log}" 2>/dev/null && \
        echo "boot log kept at ${E7FX_LOG:-/tmp/linux_futex_probe_boot.log}"
fi
[ "$fails" -eq 0 ]
