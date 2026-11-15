#!/usr/bin/env bash
# m4_ckpt_probe_check.sh — M4.6's live thread-group checkpoint/restore.
#
# The gate profile is the only Linux tenant whose task name is already in an
# EnvCkptRecord. This build swaps that tenant for fixtures/m4_ckpt_probe.S:
# the leader clones a thread, the child parks in an untimed private FUTEX_WAIT,
# the parent parks in a timed private FUTEX_WAIT, and the harness checkpoints
# only after BOTH sidecars have logged `wait (futex)`.
#
# Boot 2 replays the record and pours the payload. The acquired proof is the
# child's untimed waiter surviving the reboot: the parent's captured timeout
# expires, the parent issues FUTEX_WAKE, and the restored child writes
# M4.6-FUTEX-WOKE followed by the parent's M4.6-RESTORED-OK.
#
# Usage:
#   bash tests/m4_ckpt_probe_check.sh                 # build + two boots
#   M4_CKPT_BUILD=0 bash ...                          # use an existing image
#   M4_CKPT_ARTIFACTS=/tmp/dir bash ...               # keep evidence
#
# Exit 0 only after every clause is observed. The image is built with:
#   E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe M4_CKPT_PROBE=1

set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT" || exit 2

ISO="${M4_CKPT_ISO:-sls_m4_ckpt.iso}"
BUILD="${M4_CKPT_BUILD:-1}"
ART="${M4_CKPT_ARTIFACTS:-/tmp/m4-ckpt-probe-$$}"
L1="$ART/boot1.log"
L2="$ART/boot2.log"
IMG="$ART/nvme.img"
TOKEN=deadbeef01234567cafebabe76543210
ENTRY="${M4_CKPT_ENTRY:-3}"

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
    echo "=== build: x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe M4_CKPT_PROBE=1 X86_E3_ISO=$ISO ==="
    make x86-iso-e3 E3_INIT_FEATURES=e3_envs,e7_gate,m4_ckpt_probe \
        M4_CKPT_PROBE=1 X86_E3_ISO="$ISO" || fail "the probe image did not build"
fi
[ -f "$ROOT/$ISO" ] || fail "no image at $ISO (M4_CKPT_BUILD=0 needs a prebuilt one)"

# Source-level clauses: the task name is one constant, the fixture is the
# committed bytes the module includes, and the capture carries the group.
grep -qF 'pub const M4_CKPT_TASK_NAME: &str = "m4.ckpt.probe.0"' \
    user/proto/src/env_proto.rs || fail "the probe task name constant is missing"
grep -qF 'aerosls_proto::env_proto::M4_CKPT_TASK_NAME' user/init/src/env_manager.rs || \
    fail "init does not register the probe task name"
grep -qF 'crate::m4_ckpt_probe::spawn' user/sidecar/src/entry.rs || \
    fail "the gate profile does not select the probe spawn"
grep -qF 'linux_thread_group_refs(pds[t]->linux_tgid)' kernel/env_payload.c || \
    fail "the capture does not check the group's address-space refs"
grep -qF 'struct EnvPayloadThread' kernel/env_payload.h || \
    fail "the payload has no per-thread context array"
[ -s user/sidecar/fixtures/m4_ckpt_probe.flat ] || fail "the probe fixture is missing"

rm -f "$IMG"
truncate -s 10G "$IMG" || fail "could not make the NVMe image"

boot() {   # boot <log> <tag>
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

echo "=== boot 1: create the two-thread group, park both waiters, checkpoint ==="
: > "$L1"
boot "$L1" one
BASE="http://127.0.0.1:$PORT"

api "$ART/pcreate.json" -X POST -d '{"name":"m4ckpt"}' "$BASE/api/partitions" || \
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
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the probe spawned"
    sleep 0.5
done
[ "$spawned" -eq 1 ] || fail "the probe tenant never spawned"

ready=0
for _ in $(seq 1 480); do
    if grep -aqF "M4.6-CKPT-READY" "$L1" && \
       [ "$(grep -acF 'wait (futex)' "$L1")" -ge 2 ]; then
        ready=1; break
    fi
    grep -aqF "M4.6-CKPT-FAIL" "$L1" && fail "the probe reported a failure"
    grep -aqF "M4.6-CHILD-FAIL" "$L1" && fail "the probe's child failed"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before both futex waits parked"
    sleep 0.5
done
[ "$ready" -eq 1 ] || fail "the probe never reached READY with both futex waits parked"
grep -aF "M4.6-" "$L1" | tail -5
echo "--- futex waits: $(grep -acF 'wait (futex)' "$L1")"
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
grep -aF "capture: task" "$L1" | grep -aF "m4.ckpt.probe.0" | tail -3
if grep -aqF "EP_REFUSE_THREADS" "$L1"; then
    fail "a healthy two-thread group was refused with EP_REFUSE_THREADS"
fi

echo "=== power off; the checkpoint and its payload are on the NVMe image ==="
cleanup

echo "=== boot 2: adopt, replay, pour, and wake the restored waiter ==="
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
for _ in $(seq 1 360); do
    if grep -aqF "M4.6-RESTORED-OK" "$L2"; then ok=1; break; fi
    grep -aqF "M4.6-CKPT-FAIL" "$L2" && fail "the probe reported a failure after restore"
    grep -aqF "M4.6-CHILD-FAIL" "$L2" && fail "the child failed after restore"
    kill -0 "$QPID" 2>/dev/null || fail "QEMU exited before the restored waiter woke"
    sleep 0.5
done
[ "$ok" -eq 1 ] || fail "the parked futex waiter never survived the reboot (no M4.6-RESTORED-OK)"
grep -aqF "M4.6-FUTEX-PARENT-WAKE" "$L2" || fail "the restored parent never resumed from its captured futex wait"
grep -aqF "M4.6-FUTEX-WOKE" "$L2" || fail "the child's restored untimed futex waiter was never woken"
grep -aqF "M4.6-RESTORED-OK" "$L2" || fail "the restored parent never observed the child's wake"
if grep -aqF "M4.6-CKPT-FAIL" "$L2" || grep -aqF "M4.6-CHILD-FAIL" "$L2"; then
    fail "the restored workload reported a failure"
fi
echo "--- probe markers after restore:"
grep -aF "M4.6-" "$L2" | tail -6

cleanup
echo "PASS: two-thread group captured, payload restored, and the parked"
echo "      futex waiter woke after the reboot (M4.6-FUTEX-WOKE → M4.6-RESTORED-OK)."
echo "artifacts: $ART"
