#!/usr/bin/env bash
# m4_std_thread_probe_check.sh — M4.9's real std::thread two-boot gate.
#
# The gate profile's firstuser is a static musl Rust candidate using
# std::thread::spawn. That reaches musl pthread_create, whose stack is
# mmap(PROT_NONE) then mprotect(PROT_READ|PROT_WRITE): the PROT_NONE
# reservation must be backed on demand. Boot 1 checkpoints both parked
# contexts; boot 2 restores them, wakes the worker, and joins it.
#
# Build:
#   make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_STD_THREAD_PROBE=1 \
#       SIDECAR_E3_CPIO=sidecars_m4_std_thread_e3.cpio \
#       X86_E3_ISO=sls_m4_std_thread.iso
#
# Usage:
#   bash tests/m4_std_thread_probe_check.sh
#   M4_STD_THREAD_BUILD=0 M4_STD_THREAD_ISO=... bash ...
#
# Exit: 0 pass, 1 assertion failure, 2 missing prerequisite.

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ISO="${M4_STD_THREAD_ISO:-sls_m4_std_thread.iso}"
CPIO="${M4_STD_THREAD_CPIO:-sidecars_m4_std_thread_e3.cpio}"
BUILD="${M4_STD_THREAD_BUILD:-1}"
ART="${M4_STD_THREAD_ARTIFACTS:-/tmp/m4-std-thread-$$}"
L1="$ART/boot1.log"
L2="$ART/boot2.log"
IMG="$ART/nvme.img"
TOKEN=deadbeef01234567cafebabe76543210
ENTRY="${M4_STD_THREAD_ENTRY:-3}"

mkdir -p "$ART" || { echo "ABORT: cannot create $ART" >&2; exit 2; }

QPID=""
CATPID=""
PORT=""
cleanup() {
    [ -n "${QPID:-}" ] && kill "$QPID" 2>/dev/null
    [ -n "${CATPID:-}" ] && kill "$CATPID" 2>/dev/null
    QPID=""; CATPID=""
}
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; cleanup; exit 1; }

if [ "$BUILD" = "1" ]; then
    echo "=== build: x86-iso-e3 M4_STD_THREAD_PROBE=1 ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_STD_THREAD_PROBE=1 \
        SIDECAR_E3_CPIO="$CPIO" X86_E3_ISO="$ISO" || \
        fail "the std::thread probe image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (M4_STD_THREAD_BUILD=0 needs a prebuilt one)"

grep -qF 'std_thread_gate.elf' user/sidecar/src/m4_std_thread_probe.rs || \
    fail "the probe does not embed the std::thread fixture"
grep -qF 'm4_std_thread_probe' user/sidecar/src/lib.rs || \
    fail "the std::thread probe module is not compiled"
grep -qF 'SIDECAR_M4_STD_THREAD_PROBE' Makefile || \
    fail "the Makefile has no std::thread probe feature"

rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"

boot() {
    local log="$1" tag="$2" ser="$ART/ser_$2"
    PORT=$(bash tests/free_port.sh) || exit 2
    rm -f "$ser.in" "$ser.out"
    mkfifo "$ser.in" "$ser.out" || fail "could not make the serial pipes"
    cat "$ser.out" > "$log" &
    CATPID=$!
    qemu-system-x86_64 -cdrom "$ROOT/$ISO" \
        -drive id=disk,file="$IMG",if=none,format=raw \
        -device nvme,drive=disk,serial=slsdev0 \
        -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
        -device e1000,netdev=net0,mac=52:54:00:12:34:07 \
        -display none -m 1G -smp 2 -boot d -monitor none \
        -accel tcg,thread=multi \
        ${M4_STD_THREAD_QEMU_EXTRA:-} \
        -serial pipe:"$ser" 2> "$ART/${tag}_qemu.err" &
    QPID=$!
    bash tests/grub_select_kernel_only.sh "$ser.in" "$log" "$QPID" "$ENTRY" || \
        fail "grub entry $ENTRY did not boot ($tag)"
    local ready=0
    for _ in $(seq 1 480); do
        if grep -aq "unified=1" "$log" 2>/dev/null && \
           grep -aq "Listening on port 3000" "$log" 2>/dev/null && \
           [ "$(grep -acF '[INIT] heartbeat' "$log")" -ge 5 ]; then
            ready=1; break
        fi
        kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the control plane was up ($tag)"
        sleep 0.5
    done
    [ "$ready" -eq 1 ] || fail "the unified boot did not reach its markers ($tag)"
    echo "up($tag): control plane on $PORT"
}

api() {
    local out="$1"; shift
    local code
    code=$(curl -s --max-time 600 -w '%{http_code}' \
        -H "Authorization: Bearer $TOKEN" \
        -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null)
    echo "  [api] $* -> HTTP $code (body: $(cat "$out" 2>/dev/null))" >&2
    [ "$code" = "200" ]
}
jval() {
    python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
v = d.get(sys.argv[2], "")
print("" if v is None else v)
PY
}

echo "=== boot 1: spawn std::thread and park both waiters ==="
: > "$L1"
boot "$L1" one
BASE="http://127.0.0.1:$PORT"

api "$ART/pcreate.json" -X POST -d '{"name":"m4std"}' "$BASE/api/partitions" || \
    fail "POST /api/partitions"
PID=$(jval "$ART/pcreate.json" partition_id)
[ -n "$PID" ] && [ "$PID" != "0" ] || fail "no partition id"
echo "partition=$PID"
api "$ART/create.json" -X POST -d '{"index":1}' "$BASE/api/partition/$PID/env" || \
    fail "POST env"
echo "create: $(cat "$ART/create.json")"

spawned=0
for _ in $(seq 1 240); do
    if grep -aqF "[E7] firstuser: spawned" "$L1"; then spawned=1; break; fi
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the std::thread probe spawned"
    sleep 0.5
done
[ "$spawned" -eq 1 ] || fail "the std::thread probe never spawned"

parked=0
for _ in $(seq 1 240); do
    if grep -aqF "STD-PARENT-WAIT" "$L1" && \
       grep -aqF "STD-WORKER-WAIT" "$L1" && \
       [ "$(grep -acF 'wait (futex)' "$L1")" -ge 2 ]; then
        parked=1; break
    fi
    grep -aqF "STD-THREAD-FAIL" "$L1" && fail "the std::thread probe reported a failure"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before both waiters parked"
    sleep 0.5
done
[ "$parked" -eq 1 ] || fail "the std::thread probe never parked both futex waiters"
for marker in STD-THREAD-START STD-WORKER-TID STD-WORKER-FD-OK \
              STD-WORKER-READY STD-WORKER-WAIT STD-PARENT-WAIT; do
    grep -aqF "$marker" "$L1" || fail "missing boot-1 std::thread marker: $marker"
done
grep -aF "wait (futex)" "$L1" | tail -2

# The two Linux waiters are parked; let the system sidecars reach their own
# multi-channel recv before the quiesce samples them. M4.7's smaller capture
# happened to catch them there; the std::thread stack's extra mmap/mprotect
# traffic keeps drv.ramdisk.1 awake a little longer.
sleep 3

api "$ART/checkpoint.json" -X POST "$BASE/api/checkpoint" || \
    fail "POST /api/checkpoint"
echo "checkpoint: $(cat "$ART/checkpoint.json")"
STATUS=$(jval "$ART/checkpoint.json" status)
COUNT=$(jval "$ART/checkpoint.json" count)
[ "$STATUS" = "0" ] || fail "checkpoint status=$STATUS (want 0)"
case "$COUNT" in ''|*[!0-9]*) fail "checkpoint count='$COUNT' is not a number";; esac
[ "$COUNT" -ge 1 ] || fail "checkpoint count=$COUNT (want at least the gate record)"

captured=0
for _ in $(seq 1 120); do
    if grep -aqF "[ENV_PAYLOAD] captured partition=$PID index=1" "$L1"; then
        captured=1; break
    fi
    grep -aqF "[ENV_PAYLOAD] capture refused" "$L1" && \
        fail "the capture refused the healthy std::thread group: $(grep -aF 'capture refused' "$L1" | tail -1)"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the capture line"
    sleep 0.5
done
[ "$captured" -eq 1 ] || fail "the std::thread payload was never captured"
grep -aF "[ENV_PAYLOAD] linux group" "$L1" | tail -1 || fail "no group capture line"
if grep -aqF "EP_REFUSE_THREADS" "$L1"; then
    fail "a healthy two-thread group was refused with EP_REFUSE_THREADS"
fi

cleanup
echo "=== power off; the checkpoint and its payload are on the NVMe image ==="

echo "=== boot 2: restore, wake the worker, and join ==="
: > "$L2"
boot "$L2" two
BASE="http://127.0.0.1:$PORT"
api "$ART/restore.json" -X POST -d '{}' "$BASE/api/env/restore" || \
    fail "POST /api/env/restore did not answer (tail: $(tail -c 500 "$L2"))"
echo "restore: $(cat "$ART/restore.json")"
case "$(cat "$ART/restore.json")" in
    *'"ok":"true"'*) : ;;
    *) fail "the restore route refused the request: $(cat "$ART/restore.json")" ;;
esac
PAYLOADS=$(jval "$ART/restore.json" payloads)
SKIPS=$(jval "$ART/restore.json" payload_skips)
[ "$PAYLOADS" = "1" ] || fail "payloads=$PAYLOADS (want 1 — the group's bytes and contexts came back)"
[ "$SKIPS" = "0" ] || fail "payload_skips=$SKIPS (want 0)"
grep -aqF "[ENV_PAYLOAD] restored group" "$L2" || \
    fail "the restore did not recreate the group"
grep -aF "[ENV_PAYLOAD] restored group" "$L2" | tail -1
grep -aF "[ENV_PAYLOAD] thread" "$L2" | tail -4

ok=0
for _ in $(seq 1 480); do
    if grep -aqF "STD-THREAD-PASS" "$L2"; then ok=1; break; fi
    grep -aqF "STD-THREAD-FAIL" "$L2" && fail "the restored std::thread probe reported a failure"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the restored std::thread gate completed"
    sleep 0.5
done
[ "$ok" -eq 1 ] || fail "the restored std::thread gate never reached STD-THREAD-PASS"

for marker in STD-PARENT-WAKE=-1 STD-FUTEX-WAKE=1 STD-WORKER-WOKE \
              STD-SHARED=7 STD-JOIN-OK STD-THREAD-PASS; do
    grep -aqF "$marker" "$L2" || fail "missing restored std::thread marker: $marker"
done
if grep -aqF "[FAULT]" "$L2"; then fail "a fault appeared in the restored std::thread log"; fi
if grep -aqF "STD-THREAD-FAIL" "$L2"; then fail "the restored std::thread gate failed"; fi
grep -aF "STD-" "$L2" | tail -10

cleanup
echo "PASS: std::thread::spawn used musl pthread_create's PROT_NONE stack"
echo "      reservation and mprotect, the group survived checkpoint/restore,"
echo "      and the restored JoinHandle completed with STD-THREAD-PASS."
echo "artifacts: $ART"
