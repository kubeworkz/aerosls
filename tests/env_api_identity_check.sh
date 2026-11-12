#!/usr/bin/env bash
# tests/env_api_identity_check.sh — the failover guard's checkpoint-identity
# mismatch, against a REAL 3-node kernel cluster and the REAL HTTP route.
#
# ─── What this checks ───────────────────────────────────────────────────────
# The API reports `checkpointed` only when env_ckpt_find(partition, index)
# names the live `env_id`, and reports the record's own `env_id` separately as
# `checkpoint_env_id`. A stale record at the same (partition, index) is
# therefore visible as
#
#     checkpointed=false, checkpoint_env_id=<record id>, env_id=<live id>
#
# The failover guard must treat that reported-id mismatch as a checkpoint
# identity mismatch and abort with "checkpoint identity mismatch" and "NOT
# checkpointable" BEFORE it kills the leader. This is the real production API
# shape: the boolean is false because the record is not exact, but the
# reported record identity is still present.
#
# The stale record is injected by the kernel itself under the test-only
# ENV_CKPT_TEST_HOOKS build profile (Makefile -> AB_DEFS -> env_ckpt.c): the
# real env_ckpt_record() write keeps the manager's live id but stores
# env_id + 1000. No fake node and no test-only HTTP route is involved: this
# script boots a private cluster from that test kernel, creates a partition and
# environment through POST /api/partitions and POST /api/partition/{id}/env,
# reads the real GET /api/partition/{id}/env, then runs the real failover guard
# against the same cluster.
#
# ─── Why the private cluster ────────────────────────────────────────────────
# The guard's normal rights are to kill and relaunch nodes. This test must not
# touch an operator's cluster (or its cluster.pids), so it sets a private
# AEROSLS_CLUSTER_DIR, HTTP/console/multicast ports and log staging directory.
# It stops the cluster it started on exit; it never stops a pre-existing one.
#
# GUARD-KIND: runtime (builds a live QEMU cluster and runs the real guard; a
# source-only runner cannot satisfy it).
#
# Exit: 0 pass, 1 assertion/guard failure, 2 missing prerequisite/startup.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

TMP="$(mktemp -d)" || { echo "ABORT: mktemp -d failed." >&2; exit 2; }
CLUSTER_DIR="$TMP/cluster"
LOG_STAGING="$TMP/logs"
CLUSTER_LAUNCH_PID=""
HTTP_BASE="${AEROSLS_ENV_IDENTITY_HTTP_BASE:-59300}"
CON_BASE="${AEROSLS_ENV_IDENTITY_CON_BASE:-59320}"
HTTPS_BASE="${AEROSLS_ENV_IDENTITY_HTTPS_BASE:-59443}"
MCAST_PORT="${AEROSLS_ENV_IDENTITY_MCAST_PORT:-59340}"
TOKEN="${AEROSLS_TOKEN:-deadbeef01234567cafebabe76543210}"
RAM="${AEROSLS_ENV_IDENTITY_RAM:-768M}"
NODES=3

export AEROSLS_CLUSTER_DIR="$CLUSTER_DIR"
export AEROSLS_HTTP_BASE="$HTTP_BASE"
export AEROSLS_CON_BASE="$CON_BASE"
export AEROSLS_HTTPS_BASE="$HTTPS_BASE"
export AEROSLS_MCAST_PORT="$MCAST_PORT"
export AEROSLS_LOG_STAGING="$LOG_STAGING"
export AEROSLS_LOG_DIR="$CLUSTER_DIR"
export AEROSLS_TOKEN="$TOKEN"
# This is the only switch that makes the kernel write a stale checkpoint id.
export ENV_CKPT_TEST_HOOKS=1

cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if [ -f "$CLUSTER_DIR/cluster.pids" ]; then
        echo "==> stopping the private test cluster"
        "$ROOT/run-cluster.sh" --stop >/dev/null 2>&1 || true
        [ -n "$CLUSTER_LAUNCH_PID" ] && wait "$CLUSTER_LAUNCH_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP"
    exit "$rc"
}
trap cleanup EXIT INT TERM

command -v qemu-system-x86_64 >/dev/null 2>&1 \
    || { echo "ABORT: qemu-system-x86_64 not found." >&2; exit 2; }
command -v grub-mkrescue >/dev/null 2>&1 \
    || { echo "ABORT: grub-mkrescue not found." >&2; exit 2; }
command -v qemu-img >/dev/null 2>&1 \
    || { echo "ABORT: qemu-img not found." >&2; exit 2; }
command -v curl >/dev/null 2>&1 \
    || { echo "ABORT: curl not found." >&2; exit 2; }
command -v python3 >/dev/null 2>&1 \
    || { echo "ABORT: python3 not found." >&2; exit 2; }

AUTH=(-H "Authorization: Bearer $TOKEN")

json_get() {
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    obj = json.loads(sys.argv[1])
except Exception:
    sys.exit(1)
value = obj.get(sys.argv[2])
if value is None:
    sys.exit(1)
print(value)
PY
}

json_assert_env() {
    python3 - "$1" "$2" "$3" <<'PY'
import json, sys
body = sys.argv[1]
partition = int(sys.argv[2])
live = int(sys.argv[3])
try:
    obj = json.loads(body)
except Exception as exc:
    sys.exit("ABORT: GET /api/partition/%d/env did not return JSON: %s" % (partition, exc))
rows = [e for e in obj.get("envs", [])
        if int(e.get("partition", partition)) == partition
        and int(e.get("env_id", -1)) == live]
if len(rows) != 1:
    sys.exit("ABORT: GET /api/partition/%d/env did not list env_id=%d: %s"
             % (partition, live, body))
row = rows[0]
if str(row.get("checkpointed", "")).lower() != "false":
    sys.exit("FAIL: real API reported checkpointed=%r for stale env %d; the exact-identity API must report false here: %s"
             % (row.get("checkpointed"), live, body))
if "checkpoint_env_id" not in row:
    sys.exit("FAIL: real API withheld checkpoint_env_id for live env %d, so the stale record is not diagnosable: %s"
             % (live, body))
checkpoint_id = int(row["checkpoint_env_id"])
if checkpoint_id != live + 1000:
    sys.exit("FAIL: real API reported checkpoint_env_id=%d, expected the injected stale id %d (live %d): %s"
             % (checkpoint_id, live + 1000, live, body))
print("API OK: checkpointed=false, env_id=%d, checkpoint_env_id=%d (real stale-record mismatch)" %
      (live, checkpoint_id))
PY
}

echo "==> building and launching a private 3-node test cluster"
echo "    cluster dir: $CLUSTER_DIR"
echo "    REST base:   $HTTP_BASE"
echo "    kernel flag: ENV_CKPT_TEST_HOOKS=1"
# run-cluster.sh ends in `wait` so an operator can Ctrl-C the cluster. That is
# correct for its interactive use and wrong here: background it, keep its
# output for a failed start, and let the health wait below decide readiness.
#
# --force only bypasses the host's conservative capacity estimate. Three
# kernels are required by the guard; the host can still refuse at QEMU launch
# if it is genuinely out of memory.
"$ROOT/run-cluster.sh" --nodes "$NODES" --ram "$RAM" --smp 1 --force \
    >"$TMP/run-cluster.log" 2>&1 &
CLUSTER_LAUNCH_PID=$!

# Wait for all three nodes to answer. /api/health is deliberately unauthenticated
# in the kernel, so this also proves the management NIC forward is live.
echo "==> waiting for all $NODES nodes to answer /api/health"
deadline=$(( $(date +%s) + 300 ))
up=""
while :; do
    up=""
    for i in $(seq 1 "$NODES"); do
        port=$((HTTP_BASE + i))
        if curl -sS --max-time 2 -o /dev/null -w '%{http_code}' \
                "http://127.0.0.1:$port/api/health" 2>/dev/null | grep -qx 200; then
            up="$up $i"
        fi
    done
    [ "$(printf '%s\n' $up | grep -c .)" -eq "$NODES" ] && break
    kill -0 "$CLUSTER_LAUNCH_PID" 2>/dev/null || {
        echo "ABORT: run-cluster.sh exited before the nodes were ready. Log tail:" >&2
        tail -40 "$TMP/run-cluster.log" >&2 || true
        exit 2
    }
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 2
done
if [ "$(printf '%s\n' $up | grep -c .)" -ne "$NODES" ]; then
    echo "ABORT: only nodes'${up:- none}' answered /api/health within 300s." >&2
    tail -40 "$TMP/run-cluster.log" >&2 || true
    exit 2
fi
echo "    nodes${up} answered"

# Find the leader and wait for quorum. This is the same formation view the guard
# uses; if this fails, the guard's own formation gate would only obscure why.
leader=""
deadline=$(( $(date +%s) + 180 ))
while :; do
    leader=""
    for i in $(seq 1 "$NODES"); do
        port=$((HTTP_BASE + i))
        body="$(curl -sS --max-time 3 "${AUTH[@]}" \
                "http://127.0.0.1:$port/api/cluster" 2>/dev/null || true)"
        [ -n "$body" ] || continue
        role="$(json_get "$body" role || true)"
        active="$(json_get "$body" active_nodes || true)"
        if [ "$role" = "LEADER" ] && [ "${active:-0}" -ge 3 ] 2>/dev/null; then
            leader="$i"
            break
        fi
    done
    [ -n "$leader" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 1
done
[ -n "$leader" ] || { echo "ABORT: no leader with 3 active nodes within 180s." >&2; exit 2; }
L_PORT=$((HTTP_BASE + leader))
echo "    leader node $leader (REST $L_PORT)"

partition_name="env-identity-$$"
echo "==> creating partition '$partition_name' through the real HTTP API"
create_body="$(curl -sS --max-time 10 "${AUTH[@]}" \
    -H 'Content-Type: application/json' \
    -d "{\"name\":\"$partition_name\"}" \
    "http://127.0.0.1:$L_PORT/api/partitions")" \
    || { echo "ABORT: POST /api/partitions failed: $create_body" >&2; exit 2; }
partition_id="$(json_get "$create_body" partition_id || true)"
[ -n "$partition_id" ] \
    || { echo "ABORT: POST /api/partitions returned no partition_id: $create_body" >&2; exit 2; }
echo "    partition_id=$partition_id"

echo "==> creating an environment through POST /api/partition/$partition_id/env"
env_body="$(curl -sS --max-time 30 "${AUTH[@]}" \
    -H 'Content-Type: application/json' \
    -d '{"index":1}' \
    "http://127.0.0.1:$L_PORT/api/partition/$partition_id/env")" \
    || { echo "ABORT: POST env failed: $env_body" >&2; exit 2; }
live_env_id="$(json_get "$env_body" env_id || true)"
[ -n "$live_env_id" ] \
    || { echo "ABORT: POST env returned no env_id: $env_body" >&2; exit 2; }
echo "    live env_id=$live_env_id"

# The create returns when init has spawned the sidecars and registered the
# console; poll the actual endpoint until the row is visible, then assert the
# stale-record shape from the real serializer.
echo "==> reading the real GET /api/partition/$partition_id/env"
api_body=""
deadline=$(( $(date +%s) + 30 ))
while :; do
    api_body="$(curl -sS --max-time 3 "${AUTH[@]}" \
        "http://127.0.0.1:$L_PORT/api/partition/$partition_id/env" 2>/dev/null || true)"
    if printf '%s' "$api_body" | grep -q "\"env_id\":$live_env_id,"; then
        break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep 1
done
printf '%s' "$api_body" | grep -q "\"env_id\":$live_env_id," \
    || { echo "ABORT: real env API never listed env_id=$live_env_id: $api_body" >&2; exit 2; }
json_assert_env "$api_body" "$partition_id" "$live_env_id"

# The guard creates its own partition and environment. Because the test kernel
# injects the stale id at env_ckpt_record(), ITS environment also lands in the
# table with env_id + 1000; the API reports checkpointed=false with that
# mismatched checkpoint_env_id, and the guard's exact-identity probe must still
# diagnose the mismatch before the leader-killing step. It must not fall back
# to the fake-node path (FAKE=0).
echo "==> running the real failover guard against the live test cluster"
guard_out="$(
    AEROSLS_FAILOVER_FAST=1 \
    AEROSLS_FAILOVER_FAKE=0 \
    AEROSLS_HTTP_BASE="$HTTP_BASE" \
    AEROSLS_CLUSTER_DIR="$CLUSTER_DIR" \
    AEROSLS_LOG_DIR="$CLUSTER_DIR" \
    AEROSLS_TOKEN="$TOKEN" \
    bash tests/failover_adoption_live_check.sh 2>&1
)" && guard_rc=0 || guard_rc=$?
printf '%s\n' "$guard_out" | sed 's/^/    /'

[ "$guard_rc" -eq 2 ] || {
    echo "FAIL: the real guard exited $guard_rc, expected the identity-mismatch ABORT (2)." >&2
    exit 1
}
printf '%s\n' "$guard_out" | grep -qF 'checkpoint identity mismatch' \
    || { echo "FAIL: the real guard did not name 'checkpoint identity mismatch'." >&2; exit 1; }
printf '%s\n' "$guard_out" | grep -qF 'NOT checkpointable' \
    || { echo "FAIL: the real guard did not report 'NOT checkpointable'." >&2; exit 1; }

# The guard must abort at the env gate, not after killing the leader. A healthy
# leader endpoint after the run is the observable that it did not.
curl -sS --max-time 3 -o /dev/null -w '%{http_code}' \
    "http://127.0.0.1:$L_PORT/api/health" 2>/dev/null | grep -qx 200 \
    || { echo "FAIL: the leader stopped answering after the guard abort (it was killed)." >&2; exit 1; }
echo "GUARD OK: real cluster abort named the mismatch and the leader stayed alive"

echo "env_api_identity_check: PASS"
