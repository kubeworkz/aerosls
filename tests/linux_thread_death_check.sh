#!/usr/bin/env bash
# linux_thread_death_check.sh — E7 TG-1 I5b's live arms.
#
# I5's glue ran from exit(60) alone, so a thread that died of a fault or went
# down with exit_group never cleared its TID word and a joiner parked on it
# waited forever. Clause Z7 pins the call sites and the host test drives the
# take-once rule; this is the END-TO-END half: boot the ordinary ISO and read
# the kernel's own lines for the two deaths that exit(60) never covered.
#
#   FAULT arm (e7.tdeath.fault.0, fixtures/e7_thread_fault_probe.S):
#     a cloned thread reads a PROT_NONE page and dies of a Ring-3 #PF while a
#     sibling in its group is parked on its TID word (word ends 0x…78).
#   GROUP arm (e7.tdeath.group.0, fixtures/e7_thread_groupexit_probe.S):
#     the leader calls exit_group(231) with one thread still owning a CLEARTID
#     word (0x…58) and a robust list and a second thread parked on that word.
#
# Each arm must show, for the dying TID:
#   [E7] clear-TID: TID n word 0x…78/0x…58 cleared, joiner woken (n=1+)
#   [E7] futex wake: group g uaddr=… woken=1        (the joiner really woke)
#   [E7] robust list: TID n walked 1 entries, released 1
# and must show NO [E7] thread exit: TID n (this thread never called exit(60))
# with the clear landing before the group's one teardown line.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
set -u
cd "$(dirname "$0")/.." || exit 1

src_fails=0
sok()  { echo "ok:   $1"; }
sbad() { echo "FAIL: $1"; src_fails=$((src_fails + 1)); }

for p in e7_thread_fault_probe e7_thread_groupexit_probe; do
    [ -s "user/sidecar/fixtures/$p.flat" ] \
        && sok "the $p flat payload is present" \
        || sbad "user/sidecar/fixtures/$p.flat is missing/empty"
done
grep -qF 'E7TD-FAULT-ARM' user/sidecar/fixtures/e7_thread_fault_probe.S \
    && sok "the fault fixture carries its arm marker" \
    || sbad "the fault fixture lost E7TD-FAULT-ARM"
grep -qF 'E7TD-GROUP-ARM' user/sidecar/fixtures/e7_thread_groupexit_probe.S \
    && sok "the group fixture carries its arm marker" \
    || sbad "the group fixture lost E7TD-GROUP-ARM"
grep -qF 'pub mod e7_thread_death;' user/sidecar/src/lib.rs \
    && sok "the sidecar declares the death-arm module" \
    || sbad "user/sidecar/src/lib.rs does not declare e7_thread_death"
grep -qF 'crate::e7_thread_death::spawn_fault' user/sidecar/src/entry.rs \
    && grep -qF 'crate::e7_thread_death::spawn_group' user/sidecar/src/entry.rs \
    && sok "the sidecar spawns both death arms through the real create" \
    || sbad "entry.rs no longer spawns both death arms"
grep -qF 'FAULT_SPAWNED' user/sidecar/src/entry.rs \
    && sok "the arms retry from the event loop (PROC_MAX is full at boot)" \
    || sbad "entry.rs no longer retries the death-arm create"
grep -qF 'PROT_NONE' user/sidecar/fixtures/e7_thread_fault_probe.S \
    && sok "the fault arm dies on a PROT_NONE page, not on a mistyped escape" \
    || sbad "the fault fixture no longer maps a PROT_NONE trap page"
grep -qF '$231' user/sidecar/fixtures/e7_thread_groupexit_probe.S \
    && sok "the group arm asks for exit_group (231)" \
    || sbad "the group fixture no longer calls exit_group"

[ "$src_fails" -eq 0 ] || { echo "linux_thread_death_check: $src_fails source failure(s)"; exit 1; }

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

# Both arms must have run their set-up; the deaths follow on their own.
saw=0
for _ in $(seq 1 420); do
    if grep -aqF 'E7TD-FAULT-ARM' "$LOG" 2>/dev/null &&
       grep -aqF 'E7TD-GROUP-ARM' "$LOG" 2>/dev/null; then saw=1; break; fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
if [ "$saw" -eq 0 ]; then
    echo "FAIL: the death arms did not both reach their set-up markers in 420s"
    qemu_err
    tail -40 "$LOG" 2>/dev/null | sed 's/^/      /'
    cp "$LOG" "${E7TD_LOG:-/tmp/linux_thread_death_boot.log}" 2>/dev/null && \
        echo "      boot log kept at ${E7TD_LOG:-/tmp/linux_thread_death_boot.log}"
    exit 1
fi
# The teardowns land a few hundred ms after the markers.
sleep 8

fails=0
ok()  { echo "ok:   $1"; }
bad() { echo "FAIL: $1"; fails=$((fails + 1)); }

if grep -aqF 'E7TD-FAIL' "$LOG"; then
    bad "an arm reported E7TD-FAIL"
else
    ok "no E7TD-FAIL marker from either arm"
fi

# One arm, end to end. $1 = label, $2 = the ctid word's last two hex digits.
arm() {
    local label="$1" suffix="$2"
    local line tid gid wline gline cline gx clgroup frail
    line="$(grep -aE "\[E7\] clear-TID: TID [0-9]+ word 0x[0-9a-f]*${suffix} cleared, joiner woken \(n=[1-9]" "$LOG" | head -1)"
    if [ -z "$line" ]; then
        bad "$label: no clear-TID line whose word ends ${suffix} with a real wake count — the death path ran no glue, or it woke nobody"
        return
    fi
    tid="$(printf '%s\n' "$line" | sed -n 's/.*clear-TID: TID \([0-9][0-9]*\) .*/\1/p')"
    ok "$label: the dying thread's TID word was cleared ($line)"
    if printf '%s\n' "$line" | grep -qE '\(n=[1-9]'; then
        ok "$label: the joiner parked on that word was woken (n>=1 in the clear line)"
    else
        bad "$label: the clear reported no wake"
    fi
    if grep -aq "\[E7\] thread exit: TID $tid " "$LOG"; then
        bad "$label: TID $tid also logged an exit(60) line, so this is not the fault/exit_group path"
    else
        ok "$label: TID $tid never took the exit(60) path"
    fi
    grep -aqE "\[E7\] robust list: TID $tid walked 1 entries, released [1-9]" "$LOG" \
        && ok "$label: the robust list the dead thread owned was walked and released" \
        || bad "$label: no robust walk line for TID $tid"
    wline="$(grep -aE "\[E7\] futex wake: group [0-9]+ uaddr=0x[0-9a-f]*${suffix} woken=1" "$LOG" | head -1)"
    [ -n "$wline" ] \
        && ok "$label: the futex wake named the same word and woke exactly one waiter" \
        || bad "$label: no 'futex wake … uaddr=…${suffix} woken=1' line"
    gid="$(grep -a "\[E7\] clone: TID $tid joins group " "$LOG" | head -1 |
           sed -n 's/.*joins group \([0-9][0-9]*\).*/\1/p')"
    if [ -z "$gid" ]; then
        bad "$label: no clone line ties TID $tid to a group"
        return
    fi
    gx="$(grep -acF "[E7] group exit: group $gid " "$LOG")"
    [ "$gx" -eq 1 ] \
        && ok "$label: group $gid was torn down exactly once" \
        || bad "$label: expected one teardown of group $gid, saw $gx"
    cline="$(grep -anF "[E7] clear-TID: TID $tid word " "$LOG" | head -1 | cut -d: -f1)"
    gline="$(grep -anF "[E7] group exit: group $gid " "$LOG" | head -1 | cut -d: -f1)"
    if [ -n "$cline" ] && [ -n "$gline" ] && [ "$cline" -lt "$gline" ]; then
        ok "$label: the clear landed inside the teardown, before the group's destroy"
    else
        bad "$label: the clear did not precede the group's teardown line"
    fi
}

arm "fault arm" 78
arm "exit_group arm" 58

# The fault arm's death must be the fault: the #PF line precedes the clear.
frail="$(grep -an '\[FAULT\] Ring-3 #PF' "$LOG" | head -1 | cut -d: -f1)"
fclear="$(grep -anE '\[E7\] clear-TID: TID [0-9]+ word 0x[0-9a-f]*78 cleared' "$LOG" | head -1 | cut -d: -f1)"
if [ -n "$frail" ] && [ -n "$fclear" ] && [ "$frail" -lt "$fclear" ]; then
    ok "the fault arm's clear followed a Ring-3 #PF — the fault handler is what killed that thread"
else
    bad "no Ring-3 #PF line before the fault arm's clear"
fi
grep -aqF '[E7] linux: num=000000E7' "$LOG" \
    && ok "the group arm's exit_group (231) reached the sidecar, so that group died by request" \
    || bad "no exit_group request line ([E7] linux: num=000000E7)"

echo ""
if [ "$fails" -eq 0 ]; then
    echo "linux_thread_death_check: 0 failure(s)"
    exit 0
fi
echo "linux_thread_death_check: $fails failure(s)"
cp "$LOG" "${E7TD_LOG:-/tmp/linux_thread_death_boot.log}" 2>/dev/null && \
    echo "boot log kept at ${E7TD_LOG:-/tmp/linux_thread_death_boot.log}"
exit 1
