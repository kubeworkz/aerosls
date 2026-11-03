#!/usr/bin/env bash
# tests/partition_reuse_pause_live_check.sh — a partition id reused after a
# migration must be born RUNNING on the node that merely followed, LIVE.
#
# ─── The defect this guard exists for ─────────────────────────────────────
# partition_paused[] is indexed by SLOT, so a bit describes whichever
# partition currently occupies that slot. Only partition_destroy() used to
# clear it on the way out; partition_sync_withdraw(), the fresh-row arm of
# partition_sync_upsert() and the owned-set GC did not. A node that had the
# bit set, then learned that its partition was destroyed and the id
# re-created, kept the old occupant's pause and reported the BRAND-NEW
# partition as paused:1. `env create` there was refused by the kernel with
# `[ALLOC_REGION] CAP_EPERM: target partition N is paused — cannot charge
# it`, which the environment manager surfaced as `frame pool exhausted` --
# a diagnosis pointing at the allocator instead of the pause. Measured live
# on a real cluster; the host scenario (tests/scheduler_fairness_host_test.c,
# scenario 11) pins the same invariant over the real sync/create/destroy
# paths, but only a real cluster can prove the state crossed two kernels.
#
# ─── The live sequence, chosen so the pre-state cannot be faked ───────────
# A successful partition_migrate() leaves its SOURCE paused by design
# (`Partition remains PAUSED -- resume must happen on the destination node`).
# That is the bit the bug leaked. The guard therefore drives:
#
#   1. create P on a FOLLOWER, wait for the leader to learn it;
#   2. migrate P from that follower to the leader. The follower is now a
#      non-owner and still paused -- this guard ASSERTS paused:1 there, so
#      a pass cannot be vacuous (a migration that never paused the source
#      fails the guard at that read before the destroy);
#   3. destroy P on the leader, its owner. The follower receives the
#      withdraw and loses the row;
#   4. create a new partition on the leader. It reuses P's freed id (the
#      create path picks the lowest free slot), and the follower learns
#      the FRESH row.
#
# Post-fix the follower reports paused:0 for the new row and answers
# `env create` with `-> ok env_id=N`. Pre-fix it reports paused:1 and the
# create answers `frame pool exhausted` (the kernel's own
# `[ALLOC_REGION] ... is paused` line names the real cause). The guard
# fails on either, by name.
#
# ─── Why this does not launch a cluster ──────────────────────────────────
# Same contract as env_migrate_confirm_live_check.sh: it drives (and never
# kills) the cluster its operator started deliberately, cleans up after
# itself, and leaves the cluster running. It creates and moves only its own
# uniquely named partition; it does not sweep or destroy anything it did
# not create.
#
# ─── Usage ───────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 2          # in another shell (TCG is fine)
#   AEROSLS_TOKEN=<token> tests/partition_reuse_pause_live_check.sh
#
# A 2-node cluster suffices and is deliberate: the property is a
# disagreement between the node that ran destroy and a node that only
# followed, and two kernels are the minimum that can disagree.
#
# Environment:
#   AEROSLS_HTTP_BASE     port base (default 3000)
#   AEROSLS_TOKEN         bearer token (default: empty -- the /api reads need
#                         it; run_runtime_guards.sh exports the demo token)
#   AEROSLS_CLUSTER_DIR   where cluster.pids and node<id>.log live
#                         (default: cluster/, run-cluster.sh's)
#
# Exit: 0 pass, 1 fail (the property was violated), 2 abort (missing
# prerequisite: no cluster, no leader, no second node, no node log).
#
# GUARD-KIND: runtime
set -u
cd "$(dirname "$0")/.."

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
CTL="tools/aeroslsctl"
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-pause-reuse-$$"
NAME2="guard-pause-reuse-$$-again"

POLL=2
WAIT_NODES=300      # TCG boot + election, same generosity as the other guards
WAIT_LEARN=30       # the create announce hop
WAIT_MIGRATE=300    # aeroslsctl's own bound; the migrate blocks server-side
WAIT_WITHDRAW=60    # the withdraw announce hop
WAIT_ENV=180        # the environment manager's create round trip; the fresh-boot
                    # window can eat a client deadline (run_runtime_guards.sh's
                    # warm-up exists for exactly this), so this is a retry budget
ENV_CALL_TIMEOUT=45 # per attempt -- short enough that a cold manager cannot
                    # spend the whole budget inside one client wait

die()  { echo; echo "ABORT: $*" >&2; exit 2; }
fail() { echo; echo "FAIL  $*"; exit 1; }

command -v python3 >/dev/null 2>&1 || die "python3 is needed to parse the /api JSON."
command -v curl    >/dev/null 2>&1 || die "curl not found."
[ -f "$CTL" ] || die "$CTL not found -- run from the repo root"

TOKEN_ARGS=()
[ -n "$TOKEN" ] && TOKEN_ARGS=(--token "$TOKEN")
CURL_EXTRA=()
[ -n "$TOKEN" ] && CURL_EXTRA=(-H "Authorization: Bearer $TOKEN")

# ─── transport: curl with a bound, like every live guard ─────────────────
# A down QEMU hostfwd port accepts the TCP connect and then times out, so
# every probe needs --max-time. FETCH_STATUS/BODY carry the result for the
# caller's poll loop and the FAIL diagnostics.
FETCH_STATUS=""; FETCH_BODY=""
fetch() {   # $1 = port, $2 = path
    local out
    out="$(curl -s --max-time 3 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                "http://127.0.0.1:$1$2" 2>/dev/null)"
    FETCH_STATUS="$(printf '%s' "$out" | tail -n1)"
    FETCH_BODY="$(printf '%s' "$out" | sed '$d')"
}

get200() {   # $1 = port, $2 = path
    fetch "$1" "$2"
    case "$FETCH_STATUS" in
        200) return 0 ;;
        401|403)
            echo "ABORT: node $(( $1 - HTTP_BASE )) rejected the request" >&2
            echo "       (HTTP $FETCH_STATUS): /api routes sit behind the bearer" >&2
            echo "       token gate. Set AEROSLS_TOKEN=<token> and re-run." >&2
            exit 2 ;;
        *) return 1 ;;
    esac
}

jget() {   # $1 = json, $2 = field
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    d = {}
print(d.get(sys.argv[2], ""))
PY
}

# row_by_id <json> <id> -- "name|owner_node|paused" for that id, empty when
# the node does not carry the row. All three fields together: a stale name,
# owner or pause bit is exactly what this guard is looking for, so no caller
# should read one and infer the others.
row_by_id() {
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if str(p.get("id")) == sys.argv[2]:
        print("%s|%s|%s" % (p.get("name", ""), p.get("owner_node", ""),
                            p.get("paused", "")))
        break
PY
}

# dump_rows <json> -- every row, for the FAIL diagnostics
dump_rows() {
    python3 - "$1" <<'PY'
import json, sys
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    print("        id=%s name=%s owner=%s paused=%s" %
          (p.get("id"), p.get("name"), p.get("owner_node"), p.get("paused")))
if not rows:
    print("        (no partitions)")
PY
}

# new_bytes <file> <offset> -- only what the file gained since the offset,
# so every log assertion below is scoped to THIS run.
new_bytes() {
    tail -c "+$(( $2 + 1 ))" "$1" 2>/dev/null
}

# ─── 1. the cluster must exist, and have two nodes ───────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 2
       then re-run this guard against it."
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
NODE_COUNT="$(echo "$NODE_IDS" | wc -w)"
[ "$NODE_COUNT" -ge 2 ] || die "this guard needs at least 2 nodes (cluster.pids names: ${NODE_IDS:-none}).
       The property is a disagreement between the node that ran destroy and a
       node that only followed; one kernel cannot disagree with itself."

# ─── 2. wait for formation: a leader with quorum and one other node ──────
leader=""; other=""
echo "==> waiting for the cluster to form (up to ${WAIT_NODES}s)..."
deadline=$(( $(date +%s) + WAIT_NODES ))
while :; do
    up=""; leader=""; other=""
    for id in $NODE_IDS; do
        port=$((HTTP_BASE + id))
        if get200 "$port" /api/cluster; then
            role="$(jget "$FETCH_BODY" role)"
            active="$(jget "$FETCH_BODY" active_nodes)"
            up="$up $id"
            if [ -z "$leader" ] && [ "$role" = "LEADER" ] \
               && [ "${active:-0}" -ge 2 ] 2>/dev/null; then
                leader="$id"
            fi
        fi
    done
    for id in $up; do
        [ "$id" != "$leader" ] && [ -z "$other" ] && other="$id"
    done
    [ -n "$leader" ] && [ -n "$other" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$leader" ] || die "no leader emerged within ${WAIT_NODES}s. The nodes that
       answered:${up:- none}. Election needs a quorum -- are all nodes on
       the same segment?"
[ -n "$other" ] || die "no second node found -- the property needs a follower
       that can be left paused. Start a bigger cluster."
echo "   leader/owner node $leader, follower that will be left paused node $other"

OWNER_NODE="$leader"
STALE_NODE="$other"
O_PORT=$((HTTP_BASE + OWNER_NODE))
S_PORT=$((HTTP_BASE + STALE_NODE))
O_LOG="$CLUSTER_DIR/node$OWNER_NODE.log"
S_LOG="$CLUSTER_DIR/node$STALE_NODE.log"
[ -f "$O_LOG" ] || die "$O_LOG not found -- $CLUSTER_DIR is not this cluster's dir (AEROSLS_CLUSTER_DIR)."
[ -f "$S_LOG" ] || die "$S_LOG not found."

# ─── 3. create P on the follower; the leader must learn it ──────────────
# Created on the follower deliberately: the later migrate must run ON P's
# owner, and its source is the node whose pause bit the bug leaked. If this
# were created on the leader, the paused node after the migrate would be
# the leader -- still a valid instance of the invariant, but not the live
# shape (a follower refusing a partition it never paused).
echo "==> creating partition '$NAME' on follower node $STALE_NODE"
created=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    create_resp="$("$CTL" --host "localhost:$S_PORT" "${TOKEN_ARGS[@]}" \
        shell partition create "$NAME" 2>&1)"
    case "$create_resp" in
        *"create '$NAME' -> id="*)
            created="$(printf '%s' "$create_resp" \
                | sed -n "s/.*create '$NAME' -> id=\([0-9]*\).*/\1/p" | head -1)"
            [ -n "$created" ] && break ;;
        *"(FAILED)"*)
            die "partition create was refused on node $STALE_NODE:
$(printf '%s' "$create_resp" | tail -4 | sed 's/^/       /')" ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$created" ] || die "partition create never answered on node $STALE_NODE within
       ${WAIT_LEARN}s. Last reply:
$(printf '%s' "${create_resp:-<none>}" | tail -4 | sed 's/^/       /')"
PART_ID="$created"
echo "   partition id $PART_ID created on node $STALE_NODE"

learned=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$O_PORT" /api/partitions; then
        row="$(row_by_id "$FETCH_BODY" "$PART_ID")"
        case "$row" in
            "$NAME|$STALE_NODE|"*) learned=1; break ;;
        esac
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned" ] || fail "node $OWNER_NODE never learned partition $PART_ID ('$NAME',
       owner node $STALE_NODE) from the create announce within ${WAIT_LEARN}s.
       The migrate below would have no destination row to transfer to.
       Its list held:
$(dump_rows "$FETCH_BODY")"
echo "   node $OWNER_NODE learned it: $row"

# The bit must start clean on the follower, or step 4's paused:1 could be a
# leftover the guard did not create and the subsequent failure would be
# misattributed.
if get200 "$S_PORT" /api/partitions; then
    row="$(row_by_id "$FETCH_BODY" "$PART_ID")"
    case "$row" in
        "$NAME|$STALE_NODE|0") : ;;
        *) fail "the freshly created partition $PART_ID reads '$row' on node $STALE_NODE,
       expected '$NAME|$STALE_NODE|0'. The follower's pause bit was already set
       before this guard migrated anything, so what follows could not be
       attributed to the migration. Its list held:
$(dump_rows "$FETCH_BODY")" ;;
    esac
else
    fail "node $STALE_NODE did not answer /api/partitions before the migrate."
fi
echo "   the new partition starts running on its creator (paused:0)"

S_OFF="$(wc -c < "$S_LOG")"
O_OFF="$(wc -c < "$O_LOG")"

# ─── 4. migrate P to the leader: the follower is left paused ────────────
echo "==> migrating partition $PART_ID from node $STALE_NODE to node $OWNER_NODE"
mig_resp="$("$CTL" --host "localhost:$S_PORT" "${TOKEN_ARGS[@]}" --timeout "$WAIT_MIGRATE" \
    shell partition migrate "$PART_ID" "$OWNER_NODE" 2>&1)" \
    || die "partition migrate $PART_ID -> $OWNER_NODE failed on node $STALE_NODE --
       aeroslsctl's output was:
$(printf '%s' "$mig_resp" | tail -8 | sed 's/^/       /')"

case "$mig_resp" in
    *"migrated partition $PART_ID: node $STALE_NODE -> node $OWNER_NODE"*) ;;
    *) fail "the migrate never ran on node $STALE_NODE (the success line is absent
       from the response): $(printf '%s' "$mig_resp" | tail -4 | sed 's/^/       /')" ;;
esac
case "$mig_resp" in
    *"Partition remains PAUSED"*) ;;
    *) fail "the migrate response does not carry 'Partition remains PAUSED'. The
       source is supposed to be left paused by design (partition.c's Step 6);
       without that pause the guard has nothing to leak and would pass
       vacuously. Response:
$(printf '%s' "$mig_resp" | tail -6 | sed 's/^/       /')" ;;
esac

# Wait for both views to agree on the new owner before reading the pause.
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    s_row=""; o_row=""
    get200 "$S_PORT" /api/partitions && s_row="$(row_by_id "$FETCH_BODY" "$PART_ID")"
    get200 "$O_PORT" /api/partitions && o_row="$(row_by_id "$FETCH_BODY" "$PART_ID")"
    case "$s_row|$o_row" in
        "$NAME|$OWNER_NODE|"*"|$NAME|$OWNER_NODE|"*) break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
case "$s_row" in
    "$NAME|$OWNER_NODE|1")
        echo "   the source is left paused by the migration: $s_row" ;;
    *) fail "after the migrate, node $STALE_NODE reads '$s_row' for partition $PART_ID,
       expected '$NAME|$OWNER_NODE|1' (owner moved to node $OWNER_NODE, source
       left paused by design). The guard's pre-state is not the live defect's
       pre-state; a later pass would prove nothing." ;;
esac
case "$o_row" in
    "$NAME|$OWNER_NODE|0") echo "   the destination owns it and is running: $o_row" ;;
    *) echo "   note: destination reads '$o_row' (the source's pause is what this guard needs)" ;;
esac

# ─── 5. destroy on the owner; the follower only follows ─────────────────
echo "==> destroying partition $PART_ID on its owner, node $OWNER_NODE"
destroy_resp="$("$CTL" --host "localhost:$O_PORT" "${TOKEN_ARGS[@]}" \
    shell partition destroy "$PART_ID" 2>&1)" \
    || die "partition destroy $PART_ID failed on node $OWNER_NODE:
$(printf '%s' "$destroy_resp" | tail -6 | sed 's/^/       /')"
case "$destroy_resp" in
    *"destroy $PART_ID -> OK"*) ;;
    *) fail "the destroy of partition $PART_ID did not answer OK on its owner:
$(printf '%s' "$destroy_resp" | tail -6 | sed 's/^/       /')" ;;
esac

echo "==> waiting for node $STALE_NODE to drop the withdrawn row (up to ${WAIT_WITHDRAW}s)"
withdrawn=""
deadline=$(( $(date +%s) + WAIT_WITHDRAW ))
while :; do
    get200 "$S_PORT" /api/partitions || true
    row="$(row_by_id "$FETCH_BODY" "$PART_ID")"
    [ -z "$row" ] && { withdrawn=1; break; }
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$withdrawn" ] || fail "node $STALE_NODE still carries partition $PART_ID ($row) ${WAIT_WITHDRAW}s
       after its owner destroyed it. The withdraw announce never applied here,
       so the reuse below would race a live row. Its list held:
$(dump_rows "$FETCH_BODY")"

withdraw_line="$(new_bytes "$S_LOG" "$S_OFF" \
    | grep -F "[PARTITION] sync: partition $PART_ID withdrawn by node $OWNER_NODE." \
    | tail -1)"
[ -n "$withdraw_line" ] || fail "node $STALE_NODE dropped partition $PART_ID from its list but
       never logged '[PARTITION] sync: partition $PART_ID withdrawn by node
       $OWNER_NODE.' -- the row left by some other path, so the clear this guard
       exists for was not exercised. Its new log bytes hold:
$(new_bytes "$S_LOG" "$S_OFF" | grep -F 'PARTITION' | tail -8 | sed 's/^/       /')"
echo "   the withdraw applied on the follower: $(printf '%s' "$withdraw_line" | sed 's/^ *//')"

# ─── 6. recreate the same id; the follower must learn it RUNNING ────────
echo "==> creating a new partition on node $OWNER_NODE (must reuse id $PART_ID)"
recreate_resp="$("$CTL" --host "localhost:$O_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME2" 2>&1)" \
    || die "partition create failed on node $OWNER_NODE:
$(printf '%s' "$recreate_resp" | tail -6 | sed 's/^/       /')"
reused="$(printf '%s' "$recreate_resp" \
    | sed -n "s/.*create '$NAME2' -> id=\([0-9]*\).*/\1/p" | head -1)"
[ -n "$reused" ] || fail "could not parse the re-created partition id from:
$(printf '%s' "$recreate_resp" | tail -4 | sed 's/^/       /')"
if [ "$reused" != "$PART_ID" ]; then
    # Not a failure of the property, but the guard's scenario did not happen:
    # do not report a paused-vs-running result for a different slot.
    fail "the new partition got id $reused, not the freed id $PART_ID. The
       lowest-free-slot rule did not reuse the slot, so this run cannot speak
       about a stale bit on id $PART_ID. Check for a concurrent create."
fi
echo "   id $PART_ID reused for the brand-new partition '$NAME2'"

relearned=""; row=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    get200 "$S_PORT" /api/partitions || true
    row="$(row_by_id "$FETCH_BODY" "$PART_ID")"
    case "$row" in
        "$NAME2|$OWNER_NODE|"*) relearned=1; break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$relearned" ] || fail "node $STALE_NODE never learned the re-created partition $PART_ID
       ('$NAME2', owner node $OWNER_NODE) within ${WAIT_LEARN}s. The fresh-row
       upsert path was not exercised. Its list held:
$(dump_rows "$FETCH_BODY")"

case "$row" in
    "$NAME2|$OWNER_NODE|0")
        echo "   the reused id is born RUNNING on the follower: $row" ;;
    "$NAME2|$OWNER_NODE|1")
        fail "node $STALE_NODE reports the BRAND-NEW partition $PART_ID ('$NAME2')
       as paused:1. That is the stale-pause-bit defect: the row was withdrawn
       and re-created, but the follower kept the pause of the partition that
       used to occupy the slot. partition_sync_withdraw() and the fresh-row
       arm of partition_sync_upsert() must clear partition_paused[] on this
       node; only the node that ran the destroy did. An 'env create' for it
       will be refused next, and the shell will report the kernel's
       '[ALLOC_REGION] ... is paused' refusal as 'frame pool exhausted'." ;;
    *) fail "node $STALE_NODE reads '$row' for the re-created partition $PART_ID,
       expected '$NAME2|$OWNER_NODE|0'." ;;
esac

learn_line="$(new_bytes "$S_LOG" "$S_OFF" \
    | grep -F "[PARTITION] sync: partition $PART_ID '$NAME2' (owner node $OWNER_NODE) learned from node $OWNER_NODE." \
    | tail -1)"
[ -n "$learn_line" ] || fail "node $STALE_NODE shows the re-created row but never logged
       '[PARTITION] sync: partition $PART_ID '$NAME2' ... learned from node
       $OWNER_NODE.' -- the row did not arrive through the fresh-row upsert
       path. Its new log bytes hold:
$(new_bytes "$S_LOG" "$S_OFF" | grep -F 'PARTITION' | tail -8 | sed 's/^/       /')"
echo "   the follower learned it through the sync upsert: $(printf '%s' "$learn_line" | sed 's/^ *//')"

# ─── 7. the operator-visible proof: env create must not hit the pause ───
echo "==> creating an environment in partition $PART_ID on node $STALE_NODE"
ENV_ID=""
ENV_NOTE=0
deadline=$(( $(date +%s) + WAIT_ENV ))
while :; do
    env_resp="$("$CTL" --host "localhost:$S_PORT" "${TOKEN_ARGS[@]}" --timeout "$ENV_CALL_TIMEOUT" \
        shell env create "$PART_ID" 2>&1)"
    case "$env_resp" in
        *"-> ok env_id="*)
            ENV_ID="$(printf '%s' "$env_resp" | sed -n 's/.*ok env_id=\([0-9]*\).*/\1/p' | head -1)"
            break ;;
        *"frame pool exhausted"*|*"CAP_EPERM"*|*"is paused"*)
            fail "node $STALE_NODE REFUSED the env create for the brand-new partition
       $PART_ID, and the refusal names the stale pause:
$(printf '%s' "$env_resp" | tail -6 | sed 's/^/       /')
       The kernel's '[ALLOC_REGION] CAP_EPERM: target partition $PART_ID is
       paused — cannot charge it' is the same live failure the reused-id fix
       closes; the shell's 'frame pool exhausted' is ENV_ERR_NOMEM mapping it." ;;
        *"no reply from the environment manager"*)
            # The documented cold-boot window, not a refusal of the property:
            # the manager's sidecars are still coming up and missed the
            # client's deadline. Retry rather than fail; only an exhausted
            # budget aborts, and it says the property was not tested.
            if [ "$ENV_NOTE" -eq 0 ]; then
                echo "   the manager did not answer yet (fresh-boot window) -- retrying up to ${WAIT_ENV}s"
                ENV_NOTE=1
            fi ;;
        *"-> "*)
            fail "node $STALE_NODE's environment manager refused the env create for
       the brand-new partition $PART_ID:
$(printf '%s' "$env_resp" | tail -6 | sed 's/^/       /')" ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$ENV_ID" ] || die "node $STALE_NODE's environment manager never answered an env create
       for partition $PART_ID within ${WAIT_ENV}s (last reply:
$(printf '%s' "${env_resp:-<none>}" | tail -4 | sed 's/^/       /')). That is the documented
       fresh-boot window, not a verdict on the reused pause bit -- the guard
       cannot report the property either way; re-run once the cluster is warm."
echo "   env create answered: -> ok env_id=$ENV_ID"

# The refusal, if it happened, could have been captured in the HTTP response
# instead of the serial log (the shell's capture window). Check BOTH: the
# response was already inspected above; this catches a kernel line that did
# reach the follower's own log.
paused_refusals="$(new_bytes "$S_LOG" "$S_OFF" \
    | grep -F "[ALLOC_REGION] CAP_EPERM: target partition $PART_ID is paused" \
    | head -1)"
[ -z "$paused_refusals" ] || fail "node $STALE_NODE's log carries the kernel's stale-pause refusal
       even though the env create answered:
       $paused_refusals"
echo "   the follower logged no CAP_EPERM paused refusal for this partition"

# ─── 8. cleanup: only what this guard created ───────────────────────────
echo "==> cleaning up: ending env $ENV_ID and destroying partition $PART_ID"
"$CTL" --host "localhost:$S_PORT" "${TOKEN_ARGS[@]}" \
    shell env destroy "$PART_ID" "$ENV_ID" >/dev/null 2>&1 || true
"$CTL" --host "localhost:$O_PORT" "${TOKEN_ARGS[@]}" \
    shell partition destroy "$PART_ID" >/dev/null 2>&1 || true

echo
echo "PASS  a reused partition id is born running on a real $NODE_COUNT-node cluster:"
echo "      migration node $STALE_NODE -> node $OWNER_NODE left the source paused"
echo "      (paused:1), node $OWNER_NODE destroyed the row, the follower applied the"
echo "      withdraw, and the fresh-row upsert for the re-created id $PART_ID carried"
echo "      paused:0. An env create on the follower answered -> ok env_id=$ENV_ID, with"
echo "      no ALLOC_REGION/CAP_EPERM paused refusal in its response or its log."
echo "      (The node that ran destroy was never the bug; the nodes that only"
echo "      followed were, and this run proves they now come back running.)"
exit 0
