#!/usr/bin/env bash
# Boot the ISO under QEMU, verify the [CAP] arena carve in the boot log, and
# exercise SYS_SLS_CAP_LIST from the shell via `cap list`.
#
# GUARD-KIND: runtime (needs the built ISO + QEMU + a serial pipe).
#
# Usage:
#   bash tests/cap_boot_check.sh        # from the repo root, after make x86-iso
set -u
cd "$(dirname "$0")/.." || exit 1

SER=/tmp/sls_serial
rm -f "$SER.in" "$SER.out" boot_cap.log
mkfifo "$SER.in" "$SER.out" 2>/dev/null || true

# QEMU's pipe: chardev needs the FIFOs to pre-exist. The guest's serial
# output goes to .out; we tee it into boot_cap.log. Host->guest input goes
# through .in.
cat "$SER.out" > boot_cap.log &
CATPID=$!

qemu-system-x86_64 -cdrom sls_operating_system.iso \
    -drive id=disk,file=sls_storage.img,if=none,format=raw \
    -device nvme,drive=disk,serial=slsdev0 \
    -netdev user,id=net0,hostfwd=tcp::3001-:3000 \
    -device e1000,netdev=net0,mac=52:54:00:12:34:01 \
    -display none -m 4G -smp 4 -boot d -no-reboot \
    -serial pipe:"$SER" 2>/dev/null &
QPID=$!

# Cleanup on any exit path (including SIGTERM from run_checks.sh's per-guard
# timeout): a wedged QEMU must not outlive the check. ${VAR:-} keeps the
# trap safe under `set -u` even if it fires before the vars are set. EXIT
# and TERM/INT are separate: an `exit` inside the EXIT trap would corrupt
# the guard's normal exit status, and a signal trap must terminate the
# script (bash would otherwise resume it after the trap ran).
cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# Wait for the boot banner, then for the shell prompt.
saw_banner=0
for i in $(seq 1 120); do
    if [ -f boot_cap.log ] && grep -aq "AEROSLS BOOT LOGGER" boot_cap.log 2>/dev/null; then
        saw_banner=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
[ "$saw_banner" -eq 1 ] || { echo "FAILED: boot banner not seen"; kill "$QPID" 2>/dev/null; exit 1; }

# Wait for the shell prompt (uid:...> ) before typing.
saw_prompt=0
for i in $(seq 1 120); do
    if [ -f boot_cap.log ] && grep -aqE "uid:[0-9]+> " boot_cap.log 2>/dev/null; then
        saw_prompt=1
        break
    fi
    if ! kill -0 "$QPID" 2>/dev/null; then break; fi
    sleep 1
done
[ "$saw_prompt" -eq 1 ] || { echo "FAILED: shell prompt not seen"; kill "$QPID" 2>/dev/null; exit 1; }

# Exercise SYS_SLS_CAP_LIST, then the full Phase-1 acceptance scenario
# (alloc -> send -> recv -> write/read Hello -> revoke) from the shell.
printf 'cap list\ncap demo\n' > "$SER.in"
sleep 8
# Stop QEMU. Bounded so a wedged QEMU (SIGTERM-ignoring, stuck in D state,
# etc.) can never block the check forever: SIGTERM, 10s grace, SIGKILL.
# kill -0 succeeds on a ZOMBIE (a just-killed QEMU not yet reaped), so the
# poll also breaks on stat Z — otherwise every normal exit would burn the
# full 10s waiting on a corpse; `wait` below then reaps it instantly.
kill "$QPID" 2>/dev/null || true
for _i in $(seq 1 10); do
    kill -0 "$QPID" 2>/dev/null || break
    case "$(ps -o stat= -p "$QPID" 2>/dev/null)" in
        Z*|'') break ;;
    esac
    sleep 1
done
kill -9 "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true
kill "$CATPID" 2>/dev/null || true

fail=0
grep -aq "\[CAP\] shared-memory arena: 64 MiB physically contiguous at" boot_cap.log || {
    echo "FAILED: [CAP] arena carve line missing from the boot log" >&2
    grep -a "CAP" boot_cap.log | head -10 >&2
    fail=1
}
grep -aq "\[CAP\] Seed kernel capability layer online" boot_cap.log || {
    echo "FAILED: [CAP] online line missing from the boot log" >&2
    fail=1
}
grep -aq "\[CAP\] Capability tables:" boot_cap.log || {
    echo "FAILED: \`cap list\` did not print the capability tables" >&2
    fail=1
}
grep -aq "\[CAP-DEMO\] A reads 'Hello' back: PASS" boot_cap.log || {
    echo "FAILED: \`cap demo\` acceptance scenario did not complete (write/read PASS missing)" >&2
    grep -a "CAP-DEMO" boot_cap.log | head -20 >&2
    fail=1
}
grep -aq "\[CAP-DEMO\] revoke(MEM cap=" boot_cap.log || {
    echo "FAILED: \`cap demo\` revoke step missing" >&2
    grep -a "CAP-DEMO" boot_cap.log | head -20 >&2
    fail=1
}
[ "$fail" -eq 0 ] || { tail -40 boot_cap.log >&2; exit 1; }

echo "OK: cap_init carved the arena, SYS_SLS_CAP_LIST answered, cap demo passed live"
exit 0
