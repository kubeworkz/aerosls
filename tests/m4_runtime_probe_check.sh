#!/usr/bin/env bash
# m4_runtime_probe_check.sh — M4.7's live two-boot runtime gate.
#
# The gate profile's firstuser is the static threaded Rust candidate from
# tools/thread_runtime_gate. The fixture creates a second kernel thread,
# shares a heap object and the inherited console fd, and parks both tasks
# on the group's private futex queue. Boot 1 captures the parked group;
# boot 2 restores both contexts, wakes the worker, joins it, and requires
# M4-RUNTIME-GATE-PASS with no M4-RUNTIME-GATE-FAIL.
#
# Usage:
#   bash tests/m4_runtime_probe_check.sh                 # build + boot
#   M4_RUNTIME_BUILD=0 bash ...                          # use an existing ISO
#   M4_RUNTIME_ISO=sls_m4_runtime.iso M4_RUNTIME_ARTIFACTS=/tmp/dir ...
#
# Build:
#   make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_RUNTIME_PROBE=1 \
#       X86_E3_ISO=sls_m4_runtime.iso

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ISO="${M4_RUNTIME_ISO:-sls_m4_runtime.iso}"
BUILD="${M4_RUNTIME_BUILD:-1}"
ART="${M4_RUNTIME_ARTIFACTS:-/tmp/m4-runtime-probe-$$}"
L1="$ART/boot1.log"
L2="$ART/boot2.log"
IMG="$ART/nvme.img"
TOKEN=deadbeef01234567cafebabe76543210
ENTRY="${M4_RUNTIME_ENTRY:-3}"

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
    echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_RUNTIME_PROBE=1 X86_E3_ISO=$ISO ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate M4_RUNTIME_PROBE=1 \
        X86_E3_ISO="$ISO" || fail "the runtime probe image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (M4_RUNTIME_BUILD=0 needs a prebuilt one)"

grep -qF 'thread_runtime_gate.elf' user/sidecar/src/m4_runtime_probe.rs || \
    fail "the probe does not embed the runtime fixture"
grep -qF 'm4_runtime_probe' user/sidecar/src/lib.rs || \
    fail "the runtime probe module is not compiled"
grep -qF 'SIDECAR_M4_RUNTIME_PROBE' Makefile || \
    fail "the Makefile has no runtime probe feature"

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
        ${M4_RUNTIME_QEMU_EXTRA:-} \
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

echo "=== boot 1: spawn the threaded candidate and park both waiters ==="
: > "$L1"
boot "$L1" one
BASE="http://127.0.0.1:$PORT"

api "$ART/pcreate.json" -X POST -d '{"name":"m4rt"}' "$BASE/api/partitions" || \
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
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the runtime probe spawned"
    sleep 0.5
done
[ "$spawned" -eq 1 ] || fail "the runtime probe never spawned"

parked=0
for _ in $(seq 1 240); do
    if grep -aqF "M4-PARENT-WAIT" "$L1" && \
       grep -aqF "M4-WORKER-WAIT" "$L1" && \
       [ "$(grep -acF 'wait (futex)' "$L1")" -ge 2 ]; then
        parked=1; break
    fi
    grep -aqF "M4-RUNTIME-GATE-FAIL" "$L1" && fail "the runtime probe reported a failure"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before both waiters parked"
    sleep 0.5
done
[ "$parked" -eq 1 ] || fail "the runtime probe never parked both futex waiters"
for marker in M4-RUNTIME-START M4-WORKER-TID M4-WORKER-FD-OK \
              M4-WORKER-WAIT M4-PARENT-WAIT; do
    grep -aqF "$marker" "$L1" || fail "missing boot-1 runtime marker: $marker"
done
grep -aF "wait (futex)" "$L1" | tail -2

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
        fail "the capture refused the healthy group: $(grep -aF 'capture refused' "$L1" | tail -1)"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the capture line"
    sleep 0.5
done
[ "$captured" -eq 1 ] || fail "the two-thread payload was never captured"
grep -aF "[ENV_PAYLOAD] linux group" "$L1" | tail -1 || fail "no group capture line"
if grep -aqF "EP_REFUSE_THREADS" "$L1"; then
    fail "a healthy two-thread group was refused with EP_REFUSE_THREADS"
fi

cleanup
echo "=== power off; the checkpoint and its payload are on the NVMe image ==="

echo "=== boot 2: replay, pour, wake the restored waiter, and join ==="
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
    if grep -aqF "M4-RUNTIME-GATE-PASS" "$L2"; then ok=1; break; fi
    grep -aqF "M4-RUNTIME-GATE-FAIL" "$L2" && fail "the restored runtime probe reported a failure"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the restored runtime gate completed"
    sleep 0.5
done
[ "$ok" -eq 1 ] || fail "the restored runtime gate never reached M4-RUNTIME-GATE-PASS"

for marker in M4-PARENT-WAKE=-1 M4-FUTEX-WAKE=1 M4-WORKER-WOKE \
              M4-HEAP=7 M4-SHARED=1295267617 M4-GO-OK M4-FUTEX-OK \
              M4-CHECKPOINT-OK M4-RUNTIME-GATE-PASS; do
    grep -aqF "$marker" "$L2" || fail "missing restored runtime marker: $marker"
done
if grep -aqF "[FAULT]" "$L2"; then fail "a fault appeared in the restored runtime log"; fi
if grep -aqF "M4-RUNTIME-GATE-FAIL" "$L2"; then fail "the restored runtime gate failed"; fi
grep -aF "M4-" "$L2" | tail -8

cleanup
echo "PASS: two OS threads were created, shared a heap and fd, parked on"
echo "      the group futex, survived checkpoint/restore, and joined with"
echo "      M4-RUNTIME-GATE-PASS in boot 2."
echo "artifacts: $ART"
