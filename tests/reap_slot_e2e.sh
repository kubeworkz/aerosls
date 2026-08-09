#!/usr/bin/env bash
#
# reap_slot_e2e.sh — end-to-end test of the interrupted-migration reap.
#
# ─── What is being tested ──────────────────────────────────────────────────
# stream_migrate_recv_begin() persists the destination slot BEFORE any page
# arrives, which it must: a stream with no pages yet is otherwise lost if the
# destination reboots. The cost is a window in which a durable slot describes
# data that has not arrived, indistinguishable from a complete stream -- same
# name, same size, same frame count, over empty LBAs.
#
# The `incoming` flag closes it. Set by recv_begin, cleared only once the final
# page is written and verified, persisted either way. A slot found set at boot
# is an interrupted transfer and gets reaped, which is safe by construction:
# the SENDER retires its source only on full confirmation, so an unconfirmed
# transfer means the original still exists. The partial copy is redundant.
#
# This script forces that window open by killing the destination mid-transfer.
#
# ─── Why a script and not a list of commands ────────────────────────────────
# Four of the commands in this sequence have a shape that is easy to get wrong
# and that fails SILENTLY -- not with an error, but by doing something else and
# reporting success:
#
#   - `partition create` takes a NAME only; the id is assigned, not chosen.
#     Passing an id creates a partition called "4".
#   - A stream's partition is stamped at stream_create() from the CALLER's uid,
#     and `partition assign` is not retroactive. Create the stream before the
#     assign and it lands in partition 0 while the intended one stays empty --
#     then `partition migrate` succeeds having moved nothing, reporting
#     "0 stream(s)", which is indistinguishable from a refusal.
#   - The upload body field is `hex`, not `data`, and `raw` takes the method
#     positionally, not as `--method`.
#   - Only the OWNING node holds data to send, so the migration has to be
#     issued against the owner.
#
# So each of those is a gate here that aborts, rather than a step that might
# quietly no-op. The gates are the point of the file.
set -u

NODES="${NODES:-4}"
SRC_PORT="${SRC_PORT:-3001}"     # node 1, the source
DST_NODE="${DST_NODE:-2}"        # node 2, the destination we will kill
DST_PORT="${DST_PORT:-3002}"
CHUNKS="${CHUNKS:-64}"           # x 16 KiB = 1 MiB, wide enough to interrupt
KILL_AFTER="${KILL_AFTER:-0.5}"  # seconds into the migration
CTL="tools/aeroslsctl"
CLUSTER_DIR="cluster"
STREAM="reap-$$.bin"
PART_NAME="tenant-reap-$$"

die()  { echo; echo "ABORT: $*" >&2; exit 1; }
step() { echo; echo "── $*"; }
src()  { "$CTL" --host "localhost:$SRC_PORT" "$@"; }

command -v python3 >/dev/null || die "python3 needed to read JSON responses"
[ -x "$CTL" ] || [ -f "$CTL" ] || die "$CTL not found -- run from the repo root"

# ─── 0. the cluster must be healthy before we perturb it ───────────────────
step "0. health gate"
CL="$(src raw GET /api/cluster)" || die "node $SRC_PORT is not answering"
python3 - "$CL" <<'PY' || exit 1
import json, sys
d = json.loads(sys.argv[1])
bad = []
if str(d.get("initialised")).lower() != "true":
    bad.append("node is STANDALONE -- cluster_init was never called")
if "stack_reserved" in d and str(d["stack_reserved"]).lower() != "true":
    bad.append("stack_reserved is false: the bootstrap stack is not fully reserved")
if "stack_covered_at_boot" in d and str(d["stack_covered_at_boot"]).lower() != "true":
    bad.append("stack_covered_at_boot is false: _kernel_image_end did not cover the stack")
if int(d.get("stack_frames_withheld") or 0):
    bad.append(f"stack_frames_withheld={d['stack_frames_withheld']}: a boot reservation was wrong")
if bad:
    print("ABORT: " + "; ".join(bad), file=sys.stderr)
    print("  These are memory-integrity signals. Testing a migration on a node whose",
          file=sys.stderr)
    print("  allocator can hand out the kernel stack tests the wrong thing.", file=sys.stderr)
    sys.exit(1)
print(f"   ok: node {d.get('node_id')} {d.get('role')} term {d.get('term')}, "
      f"{d.get('active_nodes')} active, quorum {d.get('quorum_threshold')}")
PY

# ─── 0b. everything needed to restart the destination, captured UP FRONT ───
# This used to sit in step 7, after a 1 MiB upload -- so the run below wasted
# all of that work before discovering it could not proceed. A gate belongs
# before the side effects it protects, not after.
#
# And the argv comes from /proc, not from `run-cluster.sh --dry-run`. --dry-run
# refuses to run at all while a cluster is up, which is precisely when this
# needs it. Reading the command line of the process we are about to kill is
# also strictly better than re-deriving it: it is what is ACTUALLY running,
# including whatever RAM/SMP autosizing chose on this host, so the restarted
# node cannot silently differ from the one that was killed.
step "0b. capture node $DST_NODE's identity before touching anything"
DST_PID="$(awk -v n="$DST_NODE" '$1==n{print $2}' "$CLUSTER_DIR/cluster.pids" 2>/dev/null)"
[ -n "$DST_PID" ] || die "no pid for node $DST_NODE in $CLUSTER_DIR/cluster.pids"
kill -0 "$DST_PID" 2>/dev/null || die "node $DST_NODE (pid $DST_PID) is not running"

mapfile -t -d '' DST_ARGV < "/proc/$DST_PID/cmdline" \
    || die "cannot read /proc/$DST_PID/cmdline"
[ "${#DST_ARGV[@]}" -gt 3 ] || die "node $DST_NODE's argv came back with only ${#DST_ARGV[@]} element(s)"
case "${DST_ARGV[0]}" in
    *qemu*) ;;
    *) die "pid $DST_PID does not look like QEMU (argv[0]='${DST_ARGV[0]}') --
       refusing to relaunch something unidentified" ;;
esac
echo "   node $DST_NODE = pid $DST_PID, ${#DST_ARGV[@]} argv elements, ${DST_ARGV[0]##*/}"

# The disk image has to survive the kill: it is what carries the persisted
# incoming=1 slot across the restart. If it were recreated the reap would have
# nothing to find and this test would silently pass for the wrong reason.
IMG="$CLUSTER_DIR/node$DST_NODE.img"
[ -f "$IMG" ] || die "$IMG does not exist -- the persisted slot has nowhere to live"
echo "   disk image $IMG present ($(du -h "$IMG" | cut -f1)), will be reused"

# ─── 1. partition FIRST, then the uid mapping, then the stream ─────────────
step "1. create the partition (name only -- the id is assigned)"
src shell partition create "$PART_NAME" || die "partition create failed"

PID="$(src raw GET /api/partitions | python3 -c '
import json,sys
name=sys.argv[1]
for p in json.load(sys.stdin).get("partitions",[]):
    if p.get("name")==name: print(p.get("id")); break
' "$PART_NAME")"
[ -n "$PID" ] || die "could not find '$PART_NAME' in GET /api/partitions"
echo "   partition '$PART_NAME' = id $PID"

step "2. map uid 1000 (dave, the default token) to partition $PID"
# Must happen BEFORE stream_create, which stamps the partition from the caller.
src shell partition assign 1000 "$PID" || die "partition assign failed"

step "3. create the stream"
src raw POST /api/stream/create \
    --body "{\"name\":\"$STREAM\",\"mime_type\":\"application/octet-stream\"}" \
    || die "stream create failed"

step "4. GATE: the stream must actually be in partition $PID"
# GET /api/streams is the authority, not the most recent `partition assign`.
# Without this gate the migration below would report "0 stream(s)" and there
# would be no way to tell that from a refusal.
GOT="$(src raw GET /api/streams | python3 -c '
import json,sys
n=sys.argv[1]
for s in json.load(sys.stdin).get("streams",[]):
    if s.get("name")==n: print(s.get("partition_id")); break
' "$STREAM")"
[ "$GOT" = "$PID" ] || die "stream '$STREAM' is in partition '${GOT:-<not found>}', not $PID.
       The assign did not take effect before creation. Nothing after this point
       would move any data, and the migration would still report success."
echo "   ok: partition_id = $GOT"

# ─── 5. enough data that the transfer can be interrupted ───────────────────
step "5. upload $(( CHUNKS * 16 )) KiB in ${CHUNKS} x 16 KiB chunks"
HEX="$(python3 -c "print('cd'*16384)")"       # 16 KiB binary = 32 KiB hex = the max
for i in $(seq 0 $((CHUNKS-1))); do
    LAST=0; [ "$i" -eq $((CHUNKS-1)) ] && LAST=1
    src raw POST /api/stream/upload \
        --body "{\"name\":\"$STREAM\",\"hex\":\"$HEX\",\"offset\":$((i*16384)),\"last\":$LAST}" \
        >/dev/null || die "upload chunk $i failed"
    printf '\r   chunk %d/%d' "$((i+1))" "$CHUNKS"
done
echo

step "6. GATE: the source must own partition $PID"
OWNER="$(src raw GET /api/partitions | python3 -c '
import json,sys
pid=int(sys.argv[1])
for p in json.load(sys.stdin).get("partitions",[]):
    if int(p.get("id",-1))==pid: print(p.get("owner_node")); break
' "$PID")"
[ -n "$OWNER" ] || die "could not read owner_node for partition $PID"
echo "   owner_node = $OWNER"
[ "$OWNER" != "$DST_NODE" ] || die "partition $PID is already owned by node $DST_NODE"

# ─── 7. interrupt the transfer ─────────────────────────────────────────────
step "7. start the migration to node $DST_NODE, then kill it after ${KILL_AFTER}s"
src shell partition migrate "$PID" "$DST_NODE" > "$CLUSTER_DIR/migrate.out" 2>&1 &
MIG=$!
sleep "$KILL_AFTER"
kill -9 "$DST_PID" 2>/dev/null && echo "   killed node $DST_NODE (pid $DST_PID)"
wait "$MIG" 2>/dev/null
echo "   migrate output:"; sed 's/^/     /' "$CLUSTER_DIR/migrate.out"

step "8. GATE: the SOURCE must still hold the stream"
# An unconfirmed transfer must not retire the source. If this fails, the fix in
# roadmap 9k regressed and data was destroyed by an interrupted migration.
STILL="$(src raw GET /api/streams | python3 -c '
import json,sys
n=sys.argv[1]
print(any(s.get("name")==n for s in json.load(sys.stdin).get("streams",[])))
' "$STREAM")"
[ "$STILL" = "True" ] || die "the source no longer has '$STREAM'. An interrupted
       transfer retired the original -- that is data loss, not a reap."
echo "   ok: source still holds '$STREAM'"

# ─── 9. restart the destination and look for the reap ──────────────────────
step "9. restart node $DST_NODE (its log is truncated, so the reap will be at the top)"
"${DST_ARGV[@]}" </dev/null >/dev/null 2>"$CLUSTER_DIR/node$DST_NODE.stderr" &
NEW_PID=$!
echo "   relaunched as pid $NEW_PID"

# Keep cluster.pids honest, or './run-cluster.sh --stop' later kills the pid we
# just replaced and leaves this node running -- and the next launch then refuses
# to start, blaming "nodes from a previous run".
if [ -f "$CLUSTER_DIR/cluster.pids" ]; then
    awk -v n="$DST_NODE" -v p="$NEW_PID" \
        '$1==n {print n, p; next} {print}' "$CLUSTER_DIR/cluster.pids" \
        > "$CLUSTER_DIR/cluster.pids.new" \
        && mv "$CLUSTER_DIR/cluster.pids.new" "$CLUSTER_DIR/cluster.pids"
    echo "   cluster.pids updated, so --stop still works"
fi

# Wait for it to answer rather than sleeping a guessed interval. A fixed sleep
# either wastes time or reports "no reap found" for a node that had not finished
# booting -- which is a wrong answer, not a slow one.
printf '   waiting for node %s on port %s' "$DST_NODE" "$DST_PORT"
BOOTED=0
for _ in $(seq 1 40); do
    if "$CTL" --host "localhost:$DST_PORT" raw GET /api/cluster >/dev/null 2>&1; then
        BOOTED=1; break
    fi
    kill -0 "$NEW_PID" 2>/dev/null || die "node $DST_NODE died during boot. stderr:
$(sed 's/^/       /' "$CLUSTER_DIR/node$DST_NODE.stderr" | tail -5)"
    printf '.'; sleep 1
done
echo
[ "$BOOTED" -eq 1 ] || die "node $DST_NODE never answered on port $DST_PORT after 40s"
echo "   node $DST_NODE is up"

step "10. RESULT"
LOG="$CLUSTER_DIR/node$DST_NODE.log"
if grep -q 'REAPED slot' "$LOG" 2>/dev/null; then
    echo "PASS -- the interrupted slot was reaped:"
    grep -A4 'REAPED slot' "$LOG" | sed 's/^/     /'
else
    echo "NO REAP FOUND. That is not automatically a failure -- it means the"
    echo "transfer never got past BEGIN, or it finished before the kill. Check:"
    echo
    grep -nE 'STREAM|FRAME|FAULT' "$LOG" 2>/dev/null | tail -20 | sed 's/^/     /'
    echo
    echo "  If the migration COMPLETED, raise CHUNKS or lower KILL_AFTER."
    echo "  If nothing arrived at all, lower KILL_AFTER -- the kill landed"
    echo "  before BEGIN was persisted, so there was no slot to reap."
    exit 1
fi
