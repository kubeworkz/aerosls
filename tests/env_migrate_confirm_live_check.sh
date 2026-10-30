#!/usr/bin/env bash
# tests/env_migrate_confirm_live_check.sh — the confirmation round trip,
# LIVE: when a partition carrying a real environment migrates, the
# destination rebuilds it from the wire, and the source tears its half
# down ONLY on the destination's own word — so no record on the source
# describes an environment that moved.
#
# ─── Why this is the gate that matters ────────────────────────────────
# POSIX-Environments P3 moved the boundary in three increments: the
# payload before the ownership handoff (3), the destination rebuilding
# the environment from the staged slot (4), and the source letting go
# only when the destination says it is LIVE WITH ITS CONTENTS there (5:
# LIVE_REQ/LIVE_ACK on the sweep's period, teardown by name, the
# deferred frame reclamation). tests/env_migrate_host_test.c (Phases
# A–H) proves all three against the REAL wire encode/decode, the REAL
# record layer and two simulated nodes in one process. What no host
# test can see is the whole path live: two booted kernels on one L2
# segment, two inits, two NVMe disks, real sidecar create/kill round
# trips on both ends, and the BSP sweep actually driving the poll and
# the teardown. This guard is that arm — the same live-cluster gap the
# roadmap addendum names as "deliberately NOT here".
#
# ─── The evidence, each string from exactly one code site ─────────────
#   * `[ENV] create partition=.. index=.. -> ok env_id=..` — the shell's
#     E6 create (user/shell.c): the environment really existed on the
#     source before the migrate, so nothing below can be vacuous.
#   * `1 environment(s) sent and confirmed by the destination` AND
#     `frames are NOT reclaimed yet` in the migrate RESPONSE —
#     partition_migrate(): Step 3c moved the environment and Step 5
#     deferred its reclamation (the deferral line is emitted ONLY when
#     envs_moved > 0, so it cannot appear on an empty migrate).
#   * `[ENV_MIGRATE] restore: env (partition .., index ..) is LIVE on
#     node <dest>` — env_migrate.c's restore success tail, only after
#     the pour completed WITH CONTENTS (and it is the line that sets
#     the restored marker the destination's word rests on).
#   * `[ENV_MIGRATE] teardown (partition .., index ..): node <dest>
#     confirmed the restore, so env .. is ended here` — the
#     confirmation walk's destroy-OK branch: the teardown ran on the
#     destination's WORD, not on the bytes having been sent.
#   * `[ENV_MIGRATE] teardown (partition ..): every environment is
#     confirmed gone from this node -- the deferred reclamation
#     completes` — the walk completing what Step 5 deferred, and only
#     after the records are gone.
#   * `[ENV] list partition=.. live=0` on the source with `live=1` (the
#     same env) on the destination — the operator-visible half of "no
#     records describe an environment that moved": a destroyed
#     environment's console unregisters when its sidecar peer dies
#     (kernel/env_console.h), so the source's registry must empty.
#   * THE CONSOLE FOLLOWED (POSIX-Environments P3 increment 10's owed live
#     arm, its addendum's "attach on the destination, and observe the
#     stream"): a line typed into the destination's console comes back
#     through the destination's own GET — `POST .../console ok:true,
#     queued>0` then the marker echoed in `GET .../console`'s output — and
#     the SAME attach on the source is refused BY NAME (`no such
#     environment console in this partition`). The `console-unrebound`
#     defect (env_migration_check.sh's AD-a/AE teeth, source-side) leaves
#     the environment LIVE on the destination with a console that answers
#     nobody on either node — every line above still passes, which is why
#     this arm exists.
#
# ─── Why step 2b clears residue first ──────────────────────────────────
# Two failures, both caused by leftovers from earlier runs, and both
# invisible until this guard spent rounds on them:
#
# 1. THE PLACEMENT WATERMARK. The destination cannot choose where its
#    rebuilt environment lands. Placement on this OS is a deterministic
#    function of the ASCENDING first-fit watermark (kernel/frame_pool.c:
#    alloc_raw_frame() and the contiguous scan both start at the bottom
#    on every call), and the staged payload carries RAW interior pointers
#    into identity-mapped regions — so the restore's fresh create must
#    reproduce the SOURCE's recorded bases exactly, or env_migrate.c's
#    placement gate refuses the pour (ENV_MIGRATE_REFUSE_PLACEMENT,
#    "fresh environment landed off the staged bases"). ONE environment
#    left behind on either node holds low runs, every base moves
#    downrange, and the migrate is then byte-confirmed while the restore
#    still refuses: "region kind 1: staged base 0x123a5000 frames 1024,
#    fresh base 0x12a54000 frames 1024" — a delta exactly the size of
#    the residue, with the slot dropped and the LIVE polls answering
#    "not yet" until the guard times out.
#
# 2. THE CAPTURE BUFFER. /api/shell/exec answers from an 8 KiB serial
#    capture (SHELL_EXEC_OUT_CAP), bounds-checked and SILENTLY
#    truncating — everything printed after the first 8 KiB of a command
#    is discarded, and it never reaches the node's serial log either.
#    Every stale row in the partition table is re-announced on every
#    sync sweep, so a cluster carrying twenty leftovers floods that
#    buffer during the migrate and the step-5 result line
#    ("migrated partition N: node A -> node B") is gone before the guard
#    can read it — which reads as "the migrate never ran" when it ran
#    fine. Caught the same way: the response's last lines were
#    [DSPP-CKPT]/[SERVICE] noise, and node1.log had no migrate line
#    because capture suppresses the UART.
#
# So the guard clears residue on EVERY node before it creates anything:
# it ends every environment it finds (other than the env_id 0 consoles
# an E3 boot spawns, which are part of every node's identical boot
# layout) and destroys every non-system partition row, then asserts the
# precondition the gate actually compares — both nodes reporting the
# SAME ram_allocated_frames, i.e. identical allocator histories. The
# residue is TEST STATE, not the property under test; this guard drives
# (and never kills) its cluster's nodes, and it destroys nothing it did
# not find already sitting there.
#
# ─── Why it does not launch a cluster ─────────────────────────────────
# The same contract as partition_reannounce_live_check.sh: this guard
# drives (and never kills) its cluster's nodes, so it only ever touches
# a cluster its operator started deliberately, and it is safe to run
# beside a deploy gate. It cleans up after itself (env + partition
# destroyed on the destination) and leaves the cluster running.
#
# ─── Usage ────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 2          # in another shell (TCG is fine)
#   tests/env_migrate_confirm_live_check.sh
# Two nodes suffice and that is deliberate: the migrate needs both ends
# alive, and nothing here needs a third (adoption's 3-of-3 quorum is a
# different property with its own guard).
#
# Environment:
#   AEROSLS_HTTP_BASE     port base (default 3000)
#   AEROSLS_TOKEN         bearer token (default: aeroslsctl's demo
#                         DB_ADMIN token, which the kernel accepts)
#   AEROSLS_CLUSTER_DIR   where cluster.pids and node<id>.log live
#                         (default: cluster/, run-cluster.sh's)
#
# Exit: 0 pass, 1 fail (the property was violated), 2 abort (missing
# prerequisite: no cluster, no leader formed, the environment never
# existed on the source).
#
# GUARD-KIND: runtime
set -u
cd "$(dirname "$0")/.."

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
CTL="tools/aeroslsctl"
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
PID_FILE="$CLUSTER_DIR/cluster.pids"
NAME="guard-envconfirm-$$"           # unique per run, never stale

POLL=2
WAIT_NODES=300      # TCG boot + election, same generosity as the reannounce guard
WAIT_LEARN=30       # the create announce hop
WAIT_RESTORE=150    # the destination's sweep period (~10s at 1000 ticks) + create RTT
WAIT_TEARDOWN=180   # the source's poll + destroy RTT + reclamation
WAIT_ENVLIST=60     # the console registry emptying on the source
WAIT_CONSOLE=90     # a sidecar draining and echoing one line (increment 10's
                    # live arm: the console must FOLLOW the environment)
WAIT_DRAIN=60       # step 2b: residue releasing its frames + the partition
                    # tables settling (a pass or two, each a destroy + a poll)

die()  { echo; echo "ABORT: $*" >&2; exit 2; }
fail() { echo; echo "FAIL  $*"; exit 1; }

command -v python3 >/dev/null 2>&1 || die "python3 is needed to parse the /api JSON."
command -v curl    >/dev/null 2>&1 || die "curl not found."
[ -f "$CTL" ] || die "$CTL not found -- run from the repo root"

TOKEN_ARGS=()
[ -n "$TOKEN" ] && TOKEN_ARGS=(--token "$TOKEN")
CURL_EXTRA=()
[ -n "$TOKEN" ] && CURL_EXTRA=(-H "Authorization: Bearer $TOKEN")

# ─── transport: curl with a bound, like the reannounce guard ──────────
# A down QEMU hostfwd port accepts the TCP connect and then times out,
# so every probe needs --max-time. FETCH_STATUS/BODY/RC carry the result
# for the caller's poll loop and for the FAIL diagnostics.
FETCH_STATUS=""; FETCH_BODY=""; FETCH_RC=0
fetch() {   # $1 = port, $2 = path
    local out
    out="$(curl -s --max-time 3 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                "http://127.0.0.1:$1$2" 2>/dev/null)"
    FETCH_RC=$?
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

# POST_JSON/POST_STATUS -- the console's WRITE half, same bound and
# status-carrying shape as fetch(). The console route is POST with a
# JSON body; its verdict for a real delivery is ok:true AND queued>0
# (queued:0 means the environment has not drained the previous line and
# the kernel REFUSED this one -- env_console_write's pool guard), so
# every caller below checks queued, never just ok.
POST_STATUS=""; POST_BODY=""
post_json() {   # $1 = port, $2 = path, $3 = the input line
    local body out
    body="$(python3 - "$3" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
)"
    out="$(curl -s --max-time 10 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                -H 'Content-Type: application/json' -d "$body" \
                "http://127.0.0.1:$1$2" 2>/dev/null)"
    POST_STATUS="$(printf '%s' "$out" | tail -n1)"
    POST_BODY="$(printf '%s' "$out" | sed '$d')"
}

row_if() {   # $1 = json, $2 = name, $3 = owner -> "partition <id> '<name>' (owner node <o>)"
    python3 - "$1" "$2" "$3" <<'PY'
import json, sys
name, owner = sys.argv[2], sys.argv[3]
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    if p.get("name") == name and str(p.get("owner_node")) == owner:
        print("partition %s '%s' (owner node %s)" % (p.get("id"), name, owner))
        break
PY
}

dump_rows() {   # $1 = json -- every row, for the FAIL diagnostics
    python3 - "$1" <<'PY'
import json, sys
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
for p in rows:
    print("        id=%s owner=%s name=%s" % (p.get("id"), p.get("owner_node"), p.get("name")))
if not rows:
    print("        (no partitions)")
PY
}

# ─── step 2b's observables ──────────────────────────────────────────────
# /api/partitions carries every node's OWN frame_usage for each row of the
# cluster-replicated table, so one read answers both questions residue
# raises: which rows this node still carries, and what still holds frames
# here. /api/metrics carries ram_allocated_frames — the allocator history
# the placement gate is a function of.
partition_rows() {      # $1 = port -> "id:frame_usage ..." (non-system rows)
    get200 "$1" /api/partitions \
        || die "node $(( $1 - HTTP_BASE )) did not answer /api/partitions while reading residue."
    python3 - "$FETCH_BODY" <<'PY'
import json, sys
try:
    rows = json.loads(sys.argv[1]).get("partitions", [])
except Exception:
    rows = []
print(" ".join("%s:%s" % (r.get("id", 0), r.get("frame_usage", 0) or 0)
               for r in rows if int(r.get("id", 0)) != 0))
PY
}

allocated_frames() {    # $1 = port -> ram_allocated_frames
    get200 "$1" /api/metrics \
        || die "node $(( $1 - HTTP_BASE )) did not answer /api/metrics while checking allocator parity."
    jget "$FETCH_BODY" ram_allocated_frames
}

# clear_residue <port> — end every environment this node binds, then drop
# every non-system partition row it still carries. Idempotent (both readings
# are empty on a clean node) and bounded; the FAIL below is the only path
# that leaves residue standing, and it says so instead of letting the
# placement gate refuse three minutes later.
clear_residue() {        # $1 = port
    local node port="$1" pair pid eid row rows ids id deadline said=0
    node=$(( port - HTTP_BASE ))

    # Environments first: `env destroy` hands an environment's frames back
    # on its own, and prints which one it ended. env_id 0 consoles are E3
    # boot spawns (user/shell.c) — part of every node's boot layout,
    # identical on both, and not addressable — so they are left standing.
    for pair in $("$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
                      shell env list 2>/dev/null \
                  | sed -n 's/.*env partition=\([0-9][0-9]*\) env_id=\([1-9][0-9]*\).*/\1\/\2/p'); do
        pid="${pair%/*}"; eid="${pair#*/}"
        "$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
            shell env destroy "$pid" "$eid" >/dev/null 2>&1 || true
        echo "   node $node: ended the leftover environment (partition $pid, env $eid)"
    done

    # Then the partition rows. Two reasons, and the second is the one that
    # never showed up in the failure itself (the header's account):
    #   * frames — a row still charging frames holds the low runs, and the
    #     restore's fresh create then lands off the staged bases;
    #   * noise — every row still in the table is re-announced every sweep
    #     and eats the 8 KiB shell capture the migrate's result line needs.
    # A row is destroyed on the node holding it; the tables settle after a
    # pass or two, so this loops until this node reports nothing but the
    # system partition.
    deadline=$(( $(date +%s) + WAIT_DRAIN ))
    while :; do
        rows="$(partition_rows "$port")"
        [ -z "$rows" ] && break
        [ "$(date +%s)" -ge "$deadline" ] && break
        ids=""
        for row in $rows; do ids="$ids ${row%:*}"; done
        [ "$said" -eq 0 ] && echo "   node $node: destroying the leftover partition row(s):$ids" && said=1
        for id in $ids; do
            "$CTL" --host "localhost:$port" "${TOKEN_ARGS[@]}" \
                shell partition destroy "$id" >/dev/null 2>&1 || true
        done
        sleep "$POLL"
    done
    rows="$(partition_rows "$port")"
    [ -z "$rows" ] || fail "node $node still carries leftover partition row(s) ('$rows') after
       ${WAIT_DRAIN}s of destroying them. Residue there moves the placement watermark
       off the boot watermark AND floods the 8 KiB shell capture, so both halves of
       what follows are untrustworthy. The node's serial log ($CLUSTER_DIR/node$node.log)
       names what is holding them."
}

# assert_parity <when> — the precondition env_migrate.c's placement gate
# compares, checked at the moment it is load-bearing. Two nodes with the
# same ram_allocated_frames have done the same allocation work, so the
# ascending first-fit scan will hand out the same bases on both — which is
# what the staged payload's raw interior pointers require.
assert_parity() {        # $1 = when (a phrase for the failure line)
    local src dst
    src="$(allocated_frames "$L_PORT")"
    dst="$(allocated_frames "$D_PORT")"
    [ -n "$src" ] && [ -n "$dst" ] || die "could not read ram_allocated_frames (source '$src', destination '$dst')."
    if [ "$src" != "$dst" ]; then
        fail "the source and the destination disagree about their frame allocator: $src vs $dst
       allocated frames $1. Placement on this OS is a deterministic function of the
       ascending first-fit watermark (kernel/frame_pool.c scans from frame 1 on every
       call) and the staged payload carries raw interior pointers into those regions,
       so the destination's fresh create would land OFF the recorded bases and
       env_migrate.c's placement gate would refuse the pour ('fresh environment landed
       off the staged bases'). Step 2b clears residue to make these equal; if they
       still differ, something is allocating on one node only — the node's log and
       /api/partitions frame_usage will name it."
    fi
    echo "   both nodes report $src allocated frames — the histories the placement gate compares match"
}

# new_bytes <file> <offset> -- only what the file gained since the offset,
# so every log assertion below is scoped to THIS migrate and cannot pass
# on a line some earlier run left behind.
new_bytes() {
    tail -c "+$(( $2 + 1 ))" "$1" 2>/dev/null
}

# ─── 1. the cluster must exist ────────────────────────────────────────
[ -f "$PID_FILE" ] || die "no $PID_FILE -- is a cluster running? Start one first:
       ./run-cluster.sh --nodes 2
       then re-run this guard against it. (The confirmation round trip
       needs two live kernels: one to migrate FROM, one that must say the
       environment is live with its contents THERE before the source may
       let go.)"
NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || die "$PID_FILE is empty -- nothing to test."

# ─── 2. wait for formation: a leader with quorum and one other node ───
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
[ -n "$other" ] || die "no second node found -- the migrate needs a live
       destination. Start a bigger cluster."
echo "   source node $leader, destination node $other"

L_PORT=$((HTTP_BASE + leader))
D_PORT=$((HTTP_BASE + other))
ADP_LOG="$CLUSTER_DIR/node$leader.log"
DST_LOG="$CLUSTER_DIR/node$other.log"
[ -f "$ADP_LOG" ] || die "$ADP_LOG not found -- $CLUSTER_DIR is not this cluster's dir (AEROSLS_CLUSTER_DIR)."
[ -f "$DST_LOG" ] || die "$DST_LOG not found."

# ─── 2b. clear residue: both ends must sit at their boot watermark ─────
# The header carries the full account of the two failures this prevents
# (the placement gate refusing a pour whose fresh create landed past the
# recorded bases; the 8 KiB shell capture losing the migrate's result line
# to stale rows' sync traffic). Every node, not just the two used below:
# a row any node still carries is re-announced to the others, so clearing
# only the source and destination would not settle the tables.
echo "==> clearing residue on every node (the placement gate compares allocator histories)"
for id in $NODE_IDS; do
    clear_residue $(( HTTP_BASE + id ))
done

# ─── 3. create the partition on the source; the destination learns it ─
echo "==> creating partition '$NAME' on node $leader"
create_resp="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell partition create "$NAME" 2>&1)" \
    || die "partition create failed on node $leader -- aeroslsctl's output was:
$(printf '%s' "$create_resp" | tail -6 | sed 's/^/       /')"
case "$create_resp" in
    *"(FAILED)"*) die "partition create failed on node $leader: $create_resp" ;;
esac
PART_ID="$(printf '%s' "$create_resp" | sed -n "s/.*create '$NAME' -> id=\([0-9]*\).*/\1/p" | head -1)"
[ -n "$PART_ID" ] || die "could not parse the created partition id from: $(printf '%s' "$create_resp" | tail -2)"
echo "   partition id $PART_ID"

echo "==> waiting for node $other to learn partition $PART_ID (up to ${WAIT_LEARN}s)"
learned=""
deadline=$(( $(date +%s) + WAIT_LEARN ))
while :; do
    if get200 "$D_PORT" /api/partitions; then
        learned="$(row_if "$FETCH_BODY" "$NAME" "$leader")"
        [ -n "$learned" ] && break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$learned" ] || fail "node $other never learned partition $PART_ID ('$NAME') from the
       create announce within ${WAIT_LEARN}s -- DSPP announce replication is
       broken and the migrate below would race an empty destination table.
       Its list held:
$(dump_rows "$FETCH_BODY")"
echo "   $learned"

# ─── 4. create the environment on the source ──────────────────────────
# The pre-gate for everything below: an env create that fails here would
# make every later "the environment moved" line vacuous.
#
# And this is the exact moment the placement precondition must hold: the
# bases recorded in the payload are decided by the create below, and the
# destination's restore create must reproduce them later from the same
# history. `partition create` above charged no frames (a table row), so a
# difference here is residue step 2b could not clear — better named now,
# in one line, than as a refused pour after the whole transfer.
echo "==> checking that both nodes' frame allocators agree before the environment is created"
assert_parity "before the environment's bases are recorded on node $leader"

echo "==> creating an environment in partition $PART_ID on node $leader"
# The response is echoed on BOTH failure paths (the case-arm below already
# prints it): the command's own non-zero exit used to die with 'output
# above' while the output sat unprinted in $env_resp, so a transport error
# or a node refusal -- the two most interesting causes -- vanished from the
# transcript. Same fix as failover_adoption_live_check.sh's.
env_resp="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" \
    shell env create "$PART_ID" 2>&1)" \
    || die "env create failed on node $leader -- aeroslsctl's output was:
$(printf '%s' "$env_resp" | tail -6 | sed 's/^/       /')"
case "$env_resp" in
    *"-> ok env_id="*)
        ENV_ID="$(printf '%s' "$env_resp" | sed -n 's/.*ok env_id=\([0-9]*\).*/\1/p' | head -1)"
        ;;
    *)  die "the environment was not created on node $leader (no '-> ok env_id=' in the
       response): $(printf '%s' "$env_resp" | tail -5 | sed 's/^/       /')
       Without a live environment on the source there is no confirmation
       round trip to prove." ;;
esac
echo "   environment env_id=$ENV_ID lives in partition $PART_ID on node $leader"

echo "==> waiting for the environment's console to register on the source"
live_src=""
deadline=$(( $(date +%s) + WAIT_ENVLIST ))
while :; do
    list_src="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" shell env list "$PART_ID" 2>&1)"
    case "$list_src" in
        *"env partition=$PART_ID env_id=$ENV_ID"*) live_src=1; break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$live_src" ] || die "the environment's console never registered on node $leader
       ('[ENV] env partition=$PART_ID env_id=$ENV_ID' absent from env list after
       ${WAIT_ENVLIST}s). env list reads the kernel's console registry; without a
       registered console the env-list half of the evidence cannot be
       produced, and this guard refuses to pass on log lines alone when it
       promised the operator-visible half too. Last output:
$(printf '%s' "$list_src" | tail -3 | sed 's/^/       /')"
echo "   the source lists env_id=$ENV_ID as live"

# ─── 5. migrate, and capture the pre-state of both logs ───────────────
ADP_OFF="$(wc -c < "$ADP_LOG")"
DST_OFF="$(wc -c < "$DST_LOG")"
echo "==> migrating partition $PART_ID from node $leader to node $other"
# --timeout 600: a live environment's payload is ~1400 pages, each 4 wire
# fragments with an ACK round trip; on TCG that is minutes, and
# aeroslsctl's default 15s would abandon the WAIT while the migrate is
# still running server-side (the command would then read as a failure
# that never was).
mig_resp="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" --timeout 600 \
    shell partition migrate "$PART_ID" "$other" 2>&1)" \
    || die "partition migrate $PART_ID -> $other failed on node $leader --
       aeroslsctl's output was:
$(printf '%s' "$mig_resp" | tail -8 | sed 's/^/       /')"

case "$mig_resp" in
    *"migrated partition $PART_ID: node $leader -> node $other"*) ;;
    *) fail "the migrate never ran on node $leader (the success line is absent
       from the response): $(printf '%s' "$mig_resp" | tail -3 | sed 's/^/       /')" ;;
esac
case "$mig_resp" in
    *"1 environment(s) sent and confirmed by the destination"*) ;;
    *) fail "the migrate response does not report the environment as confirmed by
       the destination (no '1 environment(s) sent and confirmed by the
       destination'). The payload step did not move the environment:
$(printf '%s' "$mig_resp" | grep -m1 'environment(s)' | sed 's/^/       /')" ;;
esac
case "$mig_resp" in
    *"frames are NOT reclaimed yet"*) ;;
    *) fail "the migrate response does not carry the Step-5 deferral ('frames are
       NOT reclaimed yet'). With an environment moved, the source's frames
       MUST stay held until the destination's word -- a migrate that
       reclaimed them immediately would put two owners on one frame." ;;
esac
echo "   migrate confirmed: environment moved, reclamation deferred on the source"

# ─── 6. the destination rebuilt it (the restore line, its own log) ────
echo "==> waiting for node $other to restore the environment (up to ${WAIT_RESTORE}s; the sweep period is ~10s)"
restore_line=""
deadline=$(( $(date +%s) + WAIT_RESTORE ))
while :; do
    restore_line="$(new_bytes "$DST_LOG" "$DST_OFF" \
        | grep -F "[ENV_MIGRATE] restore: env (partition $PART_ID, index 1) is LIVE on node $other" \
        | tail -1)"
    [ -n "$restore_line" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$restore_line" ] || fail "node $other never logged the restore of (partition $PART_ID,
       index 1) within ${WAIT_RESTORE}s. The staged bytes crossed (the migrate was
       byte-confirmed) but the destination's sweep never poured them into a
       live environment. The destination's new log bytes hold:
$(new_bytes "$DST_LOG" "$DST_OFF" | grep -F "ENV_MIGRATE" | tail -8 | sed 's/^/       /')"
echo "   destination rebuilt it: $(printf '%s' "$restore_line" | sed 's/^ *//')"

# ─── 7. the source tore down ONLY on that word ────────────────────────
echo "==> waiting for the source's confirmation teardown (up to ${WAIT_TEARDOWN}s)"
teardown_line=""
deadline=$(( $(date +%s) + WAIT_TEARDOWN ))
while :; do
    teardown_line="$(new_bytes "$ADP_LOG" "$ADP_OFF" \
        | grep -F "[ENV_MIGRATE] teardown (partition $PART_ID, index 1): node $other confirmed the restore, so env $ENV_ID is ended here" \
        | tail -1)"
    [ -n "$teardown_line" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$teardown_line" ] || fail "the source node $leader never ended env $ENV_ID of partition
       $PART_ID within ${WAIT_TEARDOWN}s of the migrate (no 'node $other confirmed the
       restore' teardown line). Either the LIVE poll never reached the
       destination or the word never came back -- in both cases the source
       still DESCRIBES an environment that moved, which is the defect this
       guard exists to catch. env_migrate_pending_count() would name it; the
       source's new log bytes hold:
$(new_bytes "$ADP_LOG" "$ADP_OFF" | grep -F "ENV_MIGRATE" | tail -10 | sed 's/^/       /')"
echo "   source ended it on the word: $(printf '%s' "$teardown_line" | sed 's/^ *//')"

reclaim_line=""
deadline=$(( $(date +%s) + WAIT_TEARDOWN ))
while :; do
    reclaim_line="$(new_bytes "$ADP_LOG" "$ADP_OFF" \
        | grep -F "[ENV_MIGRATE] teardown (partition $PART_ID): every environment is confirmed gone from this node" \
        | tail -1)"
    [ -n "$reclaim_line" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ -n "$reclaim_line" ] || fail "the source never completed the deferred reclamation for
       partition $PART_ID (no 'the deferred reclamation completes' line within
       ${WAIT_TEARDOWN}s of the teardown). The records are gone but the frames are
       still held -- Step 5's deferral never got its other half. The source's
       new log bytes hold:
$(new_bytes "$ADP_LOG" "$ADP_OFF" | grep -F "ENV_MIGRATE" | tail -10 | sed 's/^/       /')"
echo "   deferred reclamation completed: $(printf '%s' "$reclaim_line" | sed 's/^ *//')"

# ─── 8. the operator-visible half: source empty, destination listing ──
echo "==> checking the console registries (source must empty, destination must list it)"
list_dst=""
deadline=$(( $(date +%s) + WAIT_ENVLIST ))
while :; do
    list_dst="$("$CTL" --host "localhost:$D_PORT" "${TOKEN_ARGS[@]}" shell env list "$PART_ID" 2>&1)"
    case "$list_dst" in
        *"env partition=$PART_ID env_id="*) break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
case "$list_dst" in
    *"env partition=$PART_ID env_id="*)
        echo "   destination lists the rebuilt environment: $(printf '%s' "$list_dst" | grep -m1 "env partition=$PART_ID" | sed 's/^ *//')"
        ;;
    *)  fail "node $other restored the environment and then lost it: 'env list $PART_ID'
       shows no environment there within ${WAIT_ENVLIST}s. The rebuilt environment is
       not reachable. Its output:
$(printf '%s' "$list_dst" | tail -3 | sed 's/^/       /')" ;;
esac

src_listed=1
deadline=$(( $(date +%s) + WAIT_ENVLIST ))
while :; do
    list_src="$("$CTL" --host "localhost:$L_PORT" "${TOKEN_ARGS[@]}" shell env list "$PART_ID" 2>&1)"
    case "$list_src" in
        *"env partition=$PART_ID env_id="*) src_listed=0 ;;
        *) src_listed=1; break ;;
    esac
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$src_listed" -eq 1 ] || fail "the source node $leader STILL lists an environment for
       partition $PART_ID after its teardown completed ('env list' output:
$(printf '%s' "$list_src" | grep "env partition=" | sed 's/^/       /')). The environment
       moved and was ended here, yet its console registry entry survives --
       a record that describes an environment that moved, at the
       operator-visible level."
echo "   the source lists nothing for partition $PART_ID -- no record of it remains"

# ─── 8b. the console FOLLOWED: attach, type, and hear it answer there ────
# POSIX-Environments P3 increment 10's owed live arm (its addendum named
# it: "boot a cluster, migrate, attach on the destination, and observe the
# stream"). Every gate above reads REGISTRIES and LOGS; none of them ever
# types into the environment. The defect this step exists to catch is a
# restore that stopped going through the create/bind round trip
# (env_migration_check.sh's AD-a/AE teeth): the environment would be LIVE
# on the destination, every line above would still pass, and its console
# would answer 'no such environment console in this partition' to
# everyone. The two halves: the destination console must ECHO a typed
# line back through its own GET (the stream is there), and the source
# console must REFUSE the same attach by name (the stream moved; it did
# not duplicate).
DST_EID="$(printf '%s' "$list_dst" | sed -n "s/.*env partition=$PART_ID env_id=\([0-9]*\).*/\1/p" | head -1)"
[ -n "$DST_EID" ] || fail "could not parse the destination's env_id from 'env list' output:
$(printf '%s' "$list_dst" | grep 'env partition=' | sed 's/^/       /')"
MARKER="P3-CONS-FOLLOWS-$$"
CONS_TR="$(mktemp)"; trap 'rm -f "$CONS_TR"' EXIT

echo "==> attaching on the destination: typing 'echo $MARKER' into (partition $PART_ID, env $DST_EID)"
posted=0
deadline=$(( $(date +%s) + WAIT_CONSOLE ))
while :; do
    post_json "$D_PORT" "/api/partition/$PART_ID/env/$DST_EID/console" "echo $MARKER"
    if [ "$POST_STATUS" = "200" ] && [ "$(jget "$POST_BODY" ok)" = "true" ] \
       && [ -n "$(jget "$POST_BODY" queued)" ] && [ "$(jget "$POST_BODY" queued)" != "0" ]; then
        posted=1; break
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$posted" -eq 1 ] || fail "the destination's console refused the line (no ok:true with
       queued>0 within ${WAIT_CONSOLE}s of POST /api/partition/$PART_ID/env/$DST_EID/console
       on node $other). The environment is LISTED there but its console does not
       answer -- the console-unrebound defect live: the restore no longer binds the
       console to the environment's new identity. Last response:
$(printf '%s' "$POST_BODY" | head -c 300)"

echo "==> waiting for the marker to come back through the destination's console (up to ${WAIT_CONSOLE}s)"
heard=0
deadline=$(( $(date +%s) + WAIT_CONSOLE ))
while :; do
    fetch "$D_PORT" "/api/partition/$PART_ID/env/$DST_EID/console"
    if [ "$FETCH_STATUS" = "200" ] && [ "$(jget "$FETCH_BODY" ok)" = "true" ]; then
        jget "$FETCH_BODY" output >> "$CONS_TR" 2>/dev/null
        # The claim is a WHOLE line: the echoed INPUT line starts with
        # 'echo' and merely contains the token, so only the shell's own
        # output (possibly carrying the '$ ' prompt on its front) counts --
        # the same whole-line rule env_checkpoint_restore_check.sh pins.
        if grep -qE "^(${MARKER}|\\\$ ${MARKER})\$" "$CONS_TR" 2>/dev/null; then
            heard=1; break
        fi
    fi
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
[ "$heard" -eq 1 ] || fail "the marker never came back through the destination's console
       (node $other, partition $PART_ID, env $DST_EID) within ${WAIT_CONSOLE}s. The line
       was queued and the environment never answered it there -- the stream did not
       follow the environment to the node it now lives on. Transcript so far:
$(tail -c 400 "$CONS_TR" 2>/dev/null | sed 's/^/       /')"
echo "   the destination's console echoed the marker -- the stream followed the environment"

# The symmetric half: the SOURCE console must refuse the same attach BY
# NAME. Its registry emptied above; this asserts the same fact at the
# ATTACH surface, where a console still bound on the node the environment
# left would otherwise hide -- a duplicate of the very record the
# teardown exists to erase.
fetch "$L_PORT" "/api/partition/$PART_ID/env/$ENV_ID/console"
if [ "$FETCH_STATUS" = "200" ] && [ "$(jget "$FETCH_BODY" ok)" = "false" ] \
   && [ "$(jget "$FETCH_BODY" error)" = "no such environment console in this partition" ]; then
    echo "   the source's console refuses by name -- the console moved, it did not duplicate"
else
    fail "the source node $leader still answers the console attach for (partition $PART_ID,
       env $ENV_ID) after its teardown (HTTP $FETCH_STATUS: $(printf '%s' "$FETCH_BODY" | head -c 200)).
       A console still bound on the node the environment left is the same
       record-that-describes-a-moved-environment, at the attach surface."
fi

# ─── 9. cleanup: the migrated pair belongs to the destination now ─────
# The DESTROY keys by the destination's OWN env_id (step 8b's DST_EID):
# the restore rebuilt the environment through create, and a create
# re-decides the manager's handle -- destroying by the source's old
# ENV_ID would silently no-op and leak the rebuilt environment and its
# partition on the destination (the `|| true` below exists for a
# genuinely-gone environment, not to hide a wrong id).
echo "==> cleaning up: destroying env ${DST_EID:-$ENV_ID} and partition $PART_ID on node $other"
"$CTL" --host "localhost:$D_PORT" "${TOKEN_ARGS[@]}" \
    shell env destroy "$PART_ID" "${DST_EID:-$ENV_ID}" >/dev/null 2>&1 || true
"$CTL" --host "localhost:$D_PORT" "${TOKEN_ARGS[@]}" \
    shell partition destroy "$PART_ID" >/dev/null 2>&1 || true

echo
echo "PASS  the confirmation round trip is proven live on a real $(( $(echo "$NODE_IDS" | wc -w) ))-node cluster:"
echo "      node $other rebuilt env $ENV_ID from the wire (restore -> LIVE), and node $leader"
echo "      ended it ONLY on node $other's word (teardown 'confirmed the restore'), then"
echo "      completed the deferred reclamation. The source no longer describes the"
echo "      environment that moved."
echo "      And the console FOLLOWED (P3 increment 10's live arm): a line typed into"
echo "      (partition $PART_ID, env $DST_EID) on node $other came back through node $other's"
echo "      own console GET, while the same attach on node $leader was refused by name."
exit 0
