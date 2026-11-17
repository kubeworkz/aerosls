#!/usr/bin/env bash
# tests/env_catalog_live_check.sh — §9.1's mediated catalog, exercised for
# real: on a RUNNING cluster, define a partition, seed its namespace with an
# object (and one object deliberately outside it), create an environment in
# that partition, and then drive `catalog info | list | stat | read` INTO that
# environment through `env attach` — requiring every answer to come back on
# the environment's own console — and then the fifth verb, `catalog sql`,
# whose read-only promise (§9.1's first cut) is asserted where it can only
# be asserted: live, with a real row-set table behind it. M5.1 then drives
# the one mediated write verb, CAT_EXEC, through a LOW-PRIVILEGE environment:
# its owner's table accepts an INSERT that a native read observes, while the
# bearer's table refuses the same environment PERM_WRITE and stays unchanged.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/env_catalog_broker_check.sh proves the §9.1 wiring from the SOURCE:
# the service is linked, the CAP exclusion is present, the manifest carries
# the fifth cap, the client chain reaches the applet. Its own header is
# explicit that these are source guards, and the roadmap named the missing
# half: "an environment that has never been booted has never made a CAT_INFO
# call". Every link in that chain is a place where the build can be right and
# the running system still wrong — a cap that resolves but is registered
# under the wrong partition, an applet that is registered but never reached
# from a console line, a reply that is sent but never drains back out. None
# of those is visible from source, and all of them fail as SILENCE: the
# tenant types `catalog info` and hears nothing. This is the guard that would
# notice.
#
# ─── What it asserts, and why each needs a running system ──────────────────
#   1. A leader answers /api/health — the control plane this guard drives is
#      live, not a stale port that still accepts a connection.
#   2. The namespace is seeded on BOTH sides of the boundary, and the foreign
#      half is proved foreign by a view the environment cannot see:
#        · `POST /api/partition/assign` puts the bearer uid in the new
#          partition, so `journal create` (owner = that uid, hence
#          partition_get_for_uid()) lands an object IN it, and `insert` puts
#          a record field on it for CAT_READ to find;
#        · `POST /api/valloc` hardcodes owner_uid 0, so it lands in
#          PARTITION_SYSTEM — a second object in the SAME catalog, outside
#          this namespace. GET /api/objects names it, which is what makes a
#          later NOENT a boundary answer rather than a missing object.
#   3. `env create` / `env list` / `env attach` reach the environment (E6's
#      surface, re-asserted because everything below depends on it): the
#      listing's env_id equals the create's, and the attach carries the
#      environment's own `[env-id] index=1 pid=…` with the pid the listing
#      reported. Without that tie, "the answer came back" could mean any
#      console in the partition.
#   4. `catalog info` through attach names THIS partition. The line is the
#      kernel's own answer (env_catalog.c's CAT_INFO reply), so a tenant can
#      verify the boundary the filtering is computed from instead of
#      trusting it. A client with no channel, or a channel registered under
#      another partition, fails here.
#   5. `catalog list 0 8` carries the in-partition object and NOT the foreign
#      one, with the header echoing the same partition id and count <= total
#      — the boundary, seen from inside.
#   6. `catalog list 99 8` comes back empty with `(empty page)`: the skip is
#      page*page_size arithmetic in the KERNEL, so a pager that ignored `skip`
#      would hand page 99 the objects page 0 had.
#   7. `catalog stat <in>` returns real metadata (type/tier/pages/owner/
#      database), and `catalog stat <out>` returns the SERVICE'S OWN refusal
#      line — "CAT_STAT refused by kernel.env.catalog — not served by the §9
#      mediated surface" — with no data after it. §9.1's contract is that a
#      refusal reaches the caller's transcript rather than silence, and this
#      transcript is the environment's console.
#   8. `catalog read <in> <key>` returns the exact value `insert` wrote; a
#      key that does not exist returns CAT_READ's refusal. The needle is the
#      VALUE, which never appears in the line this guard types, so an echoed
#      command could not satisfy it.
#   9. `catalog sql` — the SQL path (CAT_QUERY), read-only end to end:
#        · a real row-store table is seeded the documented three-step way
#          (valloc -> schema set -> table create) under the bearer uid,
#          which lands it IN this partition, and INSERT puts a run-unique
#          value on it;
#        · `catalog sql SELECT * FROM <tbl>` through attach returns a page
#          whose head counts that one row and whose cell carries the value
#          only the INSERT wrote — the engine ran, the boundary held, and
#          the reply's byte length matched its own head (the cursor's g.off);
#        · `catalog sql DROP TABLE <tbl>` is refused BY NAME with
#          "only SELECT is served on this surface", and the row is STILL
#          there afterwards — a refusal that executed would be the one bug
#          this arm exists to catch, since deny-then-execute looks exactly
#          like deny from a transcript that only shows the refusal;
#        · `catalog sql SELECT * FROM <foreign>` is answered NOENT, not PERM
#          and not rows — the partition boundary, enforced on a name the
#          SQL path resolved for itself rather than one this file typed
#          into a CAT_LIST.
#  10. `catalog exec` — the M5.1 mediated write path, end to end:
#        · a table owned by the low-privilege (bob) uid is created in the
#          same partition; a `catalog exec INSERT` from bob's environment
#          succeeds with an affected-row body and a native `sql SELECT`
#          observes the committed value (an OK reply really means committed);
#        · the same environment's `catalog exec DELETE` on dave's table is
#          refused PERM_WRITE by name, and a native read proves the row is
#          unchanged — deny-then-execute cannot pass this clause;
#        · DDL is refused by name as TYPE, and a foreign table is NOENT, so
#          the write surface cannot be used as a DDL path or an existence
#          oracle.
#
# Teeth: every assertion reads the answer out of the ENVIRONMENT's console.
# A guard that never sent a verb, sent it to the node's own shell instead, or
# matched its own command line fails. The needles are text only the kernel's
# service (or its client) can compose, never text this file writes.
#
# Prerequisite: a running cluster (./run-cluster.sh --nodes 3), curl, python3
# and AEROSLS_TOKEN — the /api routes sit behind the bearer-token gate.
#
# GUARD-KIND: runtime (needs a live cluster, never a build).
#
# Exit: 0 if every assertion held, 1 if one failed, 2 on a missing
# prerequisite (no cluster, no token, no python3, no leader).
set -u
cd "$(dirname "$0")/.."   # repo root

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
CLUSTER_DIR="${AEROSLS_CLUSTER_DIR:-cluster}"
PID_FILE="$CLUSTER_DIR/cluster.pids"

# Unique per run: the partition, both object names and the record value come
# from this shell's pid, so a crashed run can never make the next one fail
# with "already exists" or pass on a row the last one left behind.
RUN=$$
PART_NAME="e91cat-$RUN"
IN_OBJ="e91cat_in_$RUN"
OUT_OBJ="e91cat_out_$RUN"
REC_KEY="k1"
REC_VALUE="e91val-$RUN"

POLL=1
WAIT_CLUSTER=180     # election on a 3-node TCG cluster, the same generosity
                     # the other runtime guards give it
WAIT_ENVLIST=60      # the environment's console registering (E6's window)
WAIT_VERB=30         # one verb's answer draining back through env attach
WAIT_TEARDOWN=30     # the environment leaving the listing, best-effort

command -v curl    >/dev/null 2>&1 || { echo "ABORT: curl not found." >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found (JSON parsing)." >&2; exit 2; }
[ -f "$PID_FILE" ] || {
    echo "ABORT: no $PID_FILE -- is a cluster running? Start one first:" >&2
    echo "       ./run-cluster.sh --nodes 3" >&2
    exit 2
}
[ -n "$TOKEN" ] || {
    echo "ABORT: AEROSLS_TOKEN is unset -- /api routes sit behind the bearer" >&2
    echo "       token gate. Export AEROSLS_TOKEN=<token> and re-run." >&2
    exit 2
}

NODE_IDS="$(awk '{print $1}' "$PID_FILE")"
[ -n "$NODE_IDS" ] || { echo "ABORT: $PID_FILE is empty -- nothing to test." >&2; exit 2; }

CURL_EXTRA=(-H "Authorization: Bearer $TOKEN")
fail=0
L_PORT=""
NODE=""
ENV_ID=""
ENV2_ID=""
PART_ID=""
OUT_OBJ_CREATED=""
VERB_OUT=""
TEARDONE=0

# M5.0 identity arm: bob's demo token (kernel/auth.c) is a real
# low-privilege APP_USER. Its environment must not inherit the boot uid 0
# through init's sidecar tree; the denial is proved against a table owned by
# the bearer uid (dave) in the same partition. (The guest token exists but
# every POST route, including /api/shell/exec, rejects ROLE_GUEST before
# dispatch, so bob is the lowest-privilege identity the control plane lets
# through.)
PEER_TOKEN="${AEROSLS_PEER_TOKEN:-cafebabe7654321089abcdef01234567}"
PEER_UID_EXPECTED=1001

# ─── transport ────────────────────────────────────────────────────────────
# A down QEMU hostfwd port accepts the TCP connect and then times out, so
# every probe is bounded. GET_STATUS/POST_STATUS carry the HTTP code for the
# caller's own diagnostics; nothing here retries a mutation, because a
# mutation that never reached the node must be visible, not applied twice.
GET_STATUS=""; GET_BODY=""
get() {   # $1 = path
    local out
    out="$(curl -s --max-time 15 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                "http://127.0.0.1:$L_PORT$1" 2>/dev/null)"
    GET_STATUS="$(printf '%s' "$out" | tail -n1)"
    GET_BODY="$(printf '%s' "$out" | sed '$d')"
}

POST_STATUS=""; POST_BODY=""
post() {  # $1 = path, $2 = JSON body
    local out
    out="$(curl -s --max-time 15 -w '\n%{http_code}' "${CURL_EXTRA[@]}" \
                -H 'Content-Type: application/json' -d "$2" \
                "http://127.0.0.1:$L_PORT$1" 2>/dev/null)"
    POST_STATUS="$(printf '%s' "$out" | tail -n1)"
    POST_BODY="$(printf '%s' "$out" | sed '$d')"
}

jget() {  # $1 = json, $2 = field -> value or empty
    python3 - "$1" "$2" <<'PY'
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    d = {}
v = d.get(sys.argv[2])
if v is None:   print("")
elif v is True: print("true")
elif v is False:print("false")
else:           print(v)
PY
}

# ─── hard failure: name it, show what the verb returned, let the trap tear
# the run down (env + partition + the foreign object) on the way out ────────
report_and_exit() {
    echo "FAIL  the §9.1 live catalog surface (env create + attach + four verbs)" >&2
    echo "      partition='${PART_ID:-none}' env='${ENV_ID:-none}' node='${NODE:-none}'" >&2
    if [ -n "$VERB_OUT" ]; then
        echo "      last verb output:" >&2
        printf '%s\n' "$VERB_OUT" | tail -25 | sed 's/^/        /' >&2
    fi
    if [ -n "$NODE" ]; then
        echo "      kernel lines (env / catalog / partition):" >&2
        grep -a "\[ENV-CATALOG\]\|\[ENV\]\|\[PARTITION\]\|\[CATALOG\]" \
            "$CLUSTER_DIR/node$NODE.log" 2>/dev/null | tail -20 | sed 's/^/        /' >&2
    fi
    exit 1
}

die_fail() {   # $1 = message
    echo "FAILED: $1" >&2
    if [ -n "$VERB_OUT" ]; then
        printf '%s\n' "$VERB_OUT" | tail -20 | sed 's/^/        /' >&2
    fi
    fail=1
    report_and_exit
}

# ─── teardown (also the EXIT trap, so a failure mid-run leaves nothing) ───
# partition destroy kills the partition's processes and vfrees its objects,
# and clears the uid assignment made below; the foreign object lives in
# PARTITION_SYSTEM, so it needs its own vfree.
teardown() {
    [ "$TEARDONE" = 1 ] && return
    TEARDONE=1
    [ -n "$L_PORT" ] || return 0
    [ -n "$PART_ID" ] || return 0
    echo "==> teardown"
    # M5.0: the low-privilege arm demotes bob to ROLE_GUEST for its duration;
    # put the demo identity back so a rerun on the same cluster can create.
    sh_exec "role set $PEER_UID_EXPECTED APP_USER" 2>/dev/null || true
    if [ -n "$ENV_ID" ]; then
        sh_exec "env destroy $PART_ID $ENV_ID" 2>/dev/null || true
        local i
        for i in $(seq 1 60); do
            sh_exec "env list $PART_ID" 2>/dev/null || true
            printf '%s' "$SHEL_OUT" | grep -Fq "env_id=$ENV_ID" || break
            sleep 0.5
        done
        if printf '%s' "$SHEL_OUT" | grep -Fq "env_id=$ENV_ID"; then
            echo "note: env $ENV_ID still listed after 30s (the live failover guard owns that assertion)"
        else
            echo "ok:   env $ENV_ID left the listing"
        fi
    fi
    if [ -n "$ENV2_ID" ]; then
        sh_exec "env destroy $PART_ID $ENV2_ID" 2>/dev/null || true
        local i
        for i in $(seq 1 60); do
            sh_exec "env list $PART_ID" 2>/dev/null || true
            printf '%s' "$SHEL_OUT" | grep -Fq "env_id=$ENV2_ID" || break
            sleep 0.5
        done
        if printf '%s' "$SHEL_OUT" | grep -Fq "env_id=$ENV2_ID"; then
            echo "note: guest env $ENV2_ID still listed after 30s"
        else
            echo "ok:   guest env $ENV2_ID left the listing"
        fi
    fi
    sh_exec "partition destroy $PART_ID" 2>/dev/null || true
    case "$SHEL_OUT" in
        *"[PARTITION] destroy $PART_ID -> OK"*|*"[PARTITION] partition $PART_ID destroyed"*)
            echo "ok:   partition $PART_ID destroyed (its objects and the uid assignment went with it)" ;;
        *)  echo "note: partition destroy answered: $(printf '%s' "$SHEL_OUT" | tail -2 | tr '\n' ' ')" ;;
    esac
    if [ -n "$OUT_OBJ_CREATED" ]; then
        sh_exec "vfree $OUT_OBJ" 2>/dev/null || true
        get /api/objects
        if printf '%s' "$GET_BODY" | grep -Fq -- "\"$OUT_OBJ\""; then
            echo "note: '$OUT_OBJ' is still in the global catalog after vfree"
        else
            echo "ok:   the foreign object '$OUT_OBJ' was removed"
        fi
    fi
}
trap 'rc=$?; teardown; exit $rc' EXIT
trap 'exit 130' INT TERM

# ─── the shell passthrough ────────────────────────────────────────────────
# POST /api/shell/exec runs the FULL sls_shell_execute() dispatch (the same
# one the serial console drives) and returns its captured output. SHEL_OUT /
# SHEL_RECOG are what every assertion reads: `recognized:false` means the node
# did not understand the command at all, which is a failure, never a pass.
SHEL_OUT=""; SHEL_RECOG=""
sh_exec() {   # $1 = command line
    local body
    body="$(python3 - "$1" <<'PY'
import json, sys
print(json.dumps({"command": sys.argv[1]}))
PY
)"
    post /api/shell/exec "$body"
    SHEL_RECOG="$(jget "$POST_BODY" recognized)"
    SHEL_OUT="$(python3 - "$POST_BODY" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.loads(sys.argv[1])
except Exception:
    sys.exit(0)
sys.stdout.write(d.get("output") or "")
PY
)"
    SHEL_OUT="$(printf '%s' "$SHEL_OUT" | tr -d '\r')"
    [ "$SHEL_RECOG" = "false" ] && return 1
    return 0
}

# Run one shell command as a DIFFERENT authenticated identity. The HTTP
# shell session is reseeded from the bearer token on every call, so this is
# the real guest session -- not dave relabelled.
sh_exec_as() {   # $1 = token, $2 = command line
    local tok="$1" cmd="$2" rc
    local -a saved=("${CURL_EXTRA[@]}")
    CURL_EXTRA=(-H "Authorization: Bearer $tok")
    sh_exec "$cmd"
    rc=$?
    CURL_EXTRA=("${saved[@]}")
    return $rc
}

# ─── env attach: send a line, drain the answer ────────────────────────────
# `env attach <partition> <env> <line>` writes into the environment's OWN
# console (the kernel appends the terminator — env_console.c: a line needs
# its terminator or the sh applet never completes the command); a bare
# `env attach …` drains what the environment produced. env_console_write
# refuses a line while the peer still holds the previous one (the kernel's
# flow control), so the send retries exactly as
# tests/env_shell_surface_check.sh's console_send does.
ATTACH_OUT=""
ATTACH_TOKEN=""
ATTACH_ENV_ID=""
attach_shell() {   # $1 = command line, as the current attach identity
    if [ -n "$ATTACH_TOKEN" ]; then
        sh_exec_as "$ATTACH_TOKEN" "$1"
    else
        sh_exec "$1"
    fi
}
attach_drain() {
    attach_shell "env attach $PART_ID ${ATTACH_ENV_ID:-$ENV_ID}" || true
    ATTACH_OUT="$ATTACH_OUT$SHEL_OUT"
}

attach_send() {   # $1 = the line
    local t
    for t in $(seq 1 20); do
        attach_shell "env attach $PART_ID ${ATTACH_ENV_ID:-$ENV_ID} $1" || true
        case "$SHEL_OUT" in
            *"no such environment console"*)
                die_fail "env attach could not reach the environment console for '$1'" ;;
            *"input refused"*|*"queued 0 byte"*)
                sleep 1; continue ;;
            *"queued "*byte*)
                return 0 ;;
        esac
        sleep 1
    done
    die_fail "the environment never accepted '$1' through env attach (20s of refusals; last output: $SHEL_OUT)"
}

# run_verb <line> <needle> — send, then poll the drain until the needle has
# arrived AFTER the send. Everything already accumulated is stripped first,
# so an answer an earlier verb left behind cannot satisfy this one.
run_verb() {
    local before i got
    VERB_OUT=""
    attach_drain
    before="$ATTACH_OUT"
    attach_send "$1"
    for i in $(seq 1 $((WAIT_VERB * 2))); do
        attach_drain
        got="${ATTACH_OUT#"$before"}"
        if printf '%s' "$got" | grep -Fq -- "$2"; then
            VERB_OUT="$got"
            return 0
        fi
        sleep 0.5
    done
    VERB_OUT="${ATTACH_OUT#"$before"}"
    return 1
}

# expect / reject — the two assertions, each printing what it saw. Both only
# set `fail`; the run continues so one guard reports every verb at once.
expect() {   # $1 = needle, $2 = what it proves
    if printf '%s' "$VERB_OUT" | grep -Fq -- "$1"; then
        echo "ok:   $2"
    else
        echo "FAILED: $2 — expected '$1' in the environment's console output:" >&2
        printf '%s\n' "${VERB_OUT:-<nothing>}" | tail -20 | sed 's/^/        /' >&2
        fail=1
    fi
}

reject() {   # $1 = needle, $2 = what its absence proves
    if printf '%s' "$VERB_OUT" | grep -Fq -- "$1"; then
        echo "FAILED: $2 — '$1' must not appear, but it did:" >&2
        printf '%s\n' "$VERB_OUT" | tail -20 | sed 's/^/        /' >&2
        fail=1
    else
        echo "ok:   $2"
    fi
}

# ─── 1. the cluster must exist, and elect ─────────────────────────────────
echo "==> waiting for the cluster to form (up to ${WAIT_CLUSTER}s)"
deadline=$(( $(date +%s) + WAIT_CLUSTER ))
leader=""; up=""
n_nodes="$(printf '%s\n' $NODE_IDS | wc -l)"
while :; do
    up=""; leader=""
    for id in $NODE_IDS; do
        L_PORT=$((HTTP_BASE + id))   # get() talks to THIS node while we look
        get /api/cluster || continue
        [ "$GET_STATUS" = "200" ] || continue
        up="$up $id"
        role="$(jget "$GET_BODY" role)"
        active="$(jget "$GET_BODY" active_nodes)"
        if [ -z "$leader" ] && [ "$role" = "LEADER" ]; then
            if [ "$n_nodes" -lt 2 ] || [ "${active:-0}" -ge 2 ] 2>/dev/null; then
                leader="$id"
            fi
        fi
    done
    [ -n "$leader" ] && break
    [ "$(date +%s)" -ge "$deadline" ] && break
    sleep "$POLL"
done
if [ -z "$leader" ]; then
    echo "ABORT: no leader emerged within ${WAIT_CLUSTER}s. Nodes that answered:${up:- none}." >&2
    echo "       Election needs a quorum -- is the cluster up? ./run-cluster.sh --nodes 3" >&2
    exit 2
fi
NODE="$leader"
L_PORT=$((HTTP_BASE + NODE))
echo "   node $NODE answering on port $L_PORT"

get /api/health
if [ "$GET_STATUS" = "200" ] && [ "$(jget "$GET_BODY" status)" = "ok" ]; then
    echo "ok:   /api/health answers a live kernel body on node $NODE"
else
    echo "ABORT: /api/health did not answer a live body (HTTP $GET_STATUS: $GET_BODY)" >&2
    exit 2
fi

# ─── 2. a partition, then a namespace on both sides of its boundary ───────
post /api/partitions "{\"name\":\"$PART_NAME\"}"
PART_ID="$(jget "$POST_BODY" partition_id)"
if [ "$POST_STATUS" = "200" ] && [ "$(jget "$POST_BODY" ok)" = "true" ] \
   && [ -n "$PART_ID" ] && [ "$PART_ID" != "0" ] && [ "$PART_ID" != "4294967295" ]; then
    echo "ok:   POST /api/partitions defined '$PART_NAME' as partition $PART_ID"
else
    echo "ABORT: could not define partition '$PART_NAME' (HTTP $POST_STATUS: $POST_BODY)" >&2
    exit 2
fi

# The bearer token's own uid: `journal create` stamps owner_uid from the
# shell session, which api_shell_exec_post() reseeds from that token on every
# call. Assigning it to the partition is what makes the seeded object land
# INSIDE the namespace (partition_get_for_uid); the partition destroy in the
# teardown clears the assignment again.
get /api/session/whoami
SEED_UID="$(jget "$GET_BODY" uid)"
if [ -z "$SEED_UID" ]; then
    echo "ABORT: /api/session/whoami did not name a uid (HTTP $GET_STATUS: $GET_BODY)" >&2
    exit 2
fi
post /api/partition/assign "{\"uid\":$SEED_UID,\"partition_id\":$PART_ID}"
if [ "$POST_STATUS" = "200" ] && [ "$(jget "$POST_BODY" ok)" = "true" ]; then
    echo "ok:   uid $SEED_UID assigned to partition $PART_ID (the seeded object's owner resolves here)"
else
    echo "ABORT: could not assign uid $SEED_UID to partition $PART_ID (HTTP $POST_STATUS: $POST_BODY)" >&2
    exit 2
fi

# The foreign object: POST /api/valloc hardcodes owner_uid 0, so it resolves
# to PARTITION_SYSTEM whatever else has been assigned. GET /api/objects
# filters by no partition, so it is the independent proof the object exists.
post /api/valloc "{\"name\":\"$OUT_OBJ\",\"type\":1,\"pages\":1}"
OUT_OBJ_CREATED="$(jget "$POST_BODY" ok)"
get /api/objects
if [ "$OUT_OBJ_CREATED" = "true" ] && printf '%s' "$GET_BODY" | grep -Fq -- "\"$OUT_OBJ\""; then
    echo "ok:   the foreign object '$OUT_OBJ' exists in the global catalog (GET /api/objects names it)"
else
    echo "FAILED: the foreign object '$OUT_OBJ' was not created (valloc ok='$OUT_OBJ_CREATED'); without it, a later NOENT proves nothing" >&2
    echo "        POST /api/valloc said: $POST_BODY" >&2
    fail=1
fi

# The in-partition object, and a record field for CAT_READ to find.
sh_exec "journal create $IN_OBJ" \
    || die_fail "'journal create $IN_OBJ' was not recognised by the node shell"
sh_exec "insert $IN_OBJ $REC_KEY $REC_VALUE" \
    || die_fail "'insert $IN_OBJ $REC_KEY …' was not recognised by the node shell"
echo "ok:   namespace seeded: '$IN_OBJ'.$REC_KEY inside partition $PART_ID, '$OUT_OBJ' outside it"

# ─── 3. the environment, reached the way E6 proved it can be ──────────────
post "/api/partition/$PART_ID/env" '{"index":1}'
ENV_ID="$(jget "$POST_BODY" env_id)"
if [ "$POST_STATUS" = "200" ] && [ "$(jget "$POST_BODY" ok)" = "true" ] \
   && [ -n "$ENV_ID" ] && [ "$ENV_ID" != "0" ] \
   && [ "$(jget "$POST_BODY" partition)" = "$PART_ID" ]; then
    echo "ok:   POST /api/partition/$PART_ID/env created env $ENV_ID in partition $PART_ID"
else
    echo "FAILED: no environment in partition $PART_ID (HTTP $POST_STATUS: $POST_BODY)" >&2
    fail=1
    report_and_exit
fi

# The listing and the announcement, tied together: without this, "the answer
# came back" could be any console in the partition.
LIST_ROW=""; PX_PID=""
for i in $(seq 1 $((WAIT_ENVLIST * 2))); do
    sh_exec "env list $PART_ID" || true
    LIST_ROW="$(printf '%s' "$SHEL_OUT" | grep -F "[ENV] env partition=$PART_ID env_id=$ENV_ID " | head -1)"
    [ -n "$LIST_ROW" ] && break
    sleep 0.5
done
if [ -z "$LIST_ROW" ]; then
    die_fail "\`env list $PART_ID\` never reported env $ENV_ID (last output: ${SHEL_OUT:-<none>})"
fi
PX_PID="$(printf '%s' "$LIST_ROW" | grep -o 'posix_pid=[0-9]*' | cut -d= -f2)"
if [ -z "$PX_PID" ] || [ "$PX_PID" = "0" ]; then
    die_fail "the listing row carried no usable posix_pid (row: $LIST_ROW)"
fi
echo "ok:   \`env list $PART_ID\` reports env $ENV_ID with posix_pid=$PX_PID"

announce=""
for i in $(seq 1 $((WAIT_ENVLIST * 2))); do
    attach_drain
    announce="$(printf '%s' "$ATTACH_OUT" | grep -o '\[env-id\] index=[0-9]* pid=[0-9]*' | head -1)"
    [ -n "$announce" ] && break
    sleep 0.5
done
if [ -n "$announce" ] && [ "$announce" = "[env-id] index=1 pid=$PX_PID" ]; then
    echo "ok:   \`env attach $PART_ID $ENV_ID\` carries the environment's own announcement ('$announce')"
else
    die_fail "the console the shell attached to does not state this environment's identity (expected '[env-id] index=1 pid=$PX_PID', got '${announce:-nothing}')"
fi

# ─── 4. CAT_INFO — the kernel's own answer for THIS channel ───────────────
if run_verb "catalog info" "the kernel's own answer for this channel"; then
    expect "partition $PART_ID " "CAT_INFO names partition $PART_ID — the channel registered under the environment's own partition"
    expect "objects "           "CAT_INFO carries the service's real ceilings"
    reject "no catalog channel" "the environment's manifest named the catalog cap (ctx.catalog() was Some)"
else
    die_fail "\`catalog info\` through \`env attach\` never answered — nothing came back on the environment's console in ${WAIT_VERB}s"
fi

# ─── 5. CAT_LIST — the namespace, with the boundary visible ───────────────
if run_verb "catalog list 0 8" "objects (partition $PART_ID)"; then
    head_line="$(printf '%s' "$VERB_OUT" | grep -Eo "page 0: [0-9]+ of [0-9]+ objects \(partition $PART_ID\)" | head -1)"
    if [ -n "$head_line" ]; then
        echo "ok:   \`catalog list 0 8\` header is '$head_line'"
        cnt="$(printf '%s' "$head_line" | sed -n 's/^page 0: \([0-9]*\) of \([0-9]*\) objects.*/\1/p')"
        tot="$(printf '%s' "$head_line" | sed -n 's/^page 0: \([0-9]*\) of \([0-9]*\) objects.*/\2/p')"
        if [ -n "$cnt" ] && [ -n "$tot" ] && [ "$cnt" -le "$tot" ]; then
            echo "ok:   count $cnt <= total $tot — the page is a slice of the namespace it reports"
        else
            echo "FAILED: the list header reported count '$cnt' against total '$tot'" >&2
            fail=1
        fi
    else
        echo "FAILED: the list header did not report this partition's page (expected 'page 0: N of T objects (partition $PART_ID)')" >&2
        printf '%s\n' "$VERB_OUT" | tail -20 | sed 's/^/        /' >&2
        fail=1
    fi
    expect "  $IN_OBJ"  "the in-partition object is in the page"
    reject "  $OUT_OBJ" "the foreign object is NOT in the page (the boundary holds from inside)"
else
    die_fail "\`catalog list 0 8\` never answered through env attach in ${WAIT_VERB}s"
fi

# The pager's own arithmetic, in the kernel: page 99 is past every object
# this partition holds, and only an honest skip lands on empty.
if run_verb "catalog list 99 8" "objects (partition $PART_ID)"; then
    if printf '%s' "$VERB_OUT" | grep -Eq "page 99: 0 of [0-9]+ objects \(partition $PART_ID\)"; then
        echo "ok:   \`catalog list 99 8\` is empty (page 99: 0 of T) — the kernel skips by page*page_size"
    else
        echo "FAILED: page 99 was not empty — the pager did not skip to the end:" >&2
        printf '%s\n' "$VERB_OUT" | tail -20 | sed 's/^/        /' >&2
        fail=1
    fi
    expect "(empty page)" "the service says so in words, rather than returning a plausible page"
else
    die_fail "\`catalog list 99 8\` never answered through env attach in ${WAIT_VERB}s"
fi

# ─── 6. CAT_STAT — real metadata inside, a named refusal outside ──────────
if run_verb "catalog stat $IN_OBJ" "owner="; then
    if printf '%s' "$VERB_OUT" | grep -Eq "type=[0-9]+ tier=[0-9]+ pages=1 owner=[0-9]+ database=[0-9]+"; then
        echo "ok:   \`catalog stat $IN_OBJ\` returns the real metadata ($(printf '%s' "$VERB_OUT" | grep -Eo 'type=[0-9]+ tier=[0-9]+ pages=[0-9]+ owner=[0-9]+ database=[0-9]+' | head -1))"
    else
        echo "FAILED: CAT_STAT's answer was not this object's metadata:" >&2
        printf '%s\n' "$VERB_OUT" | tail -20 | sed 's/^/        /' >&2
        fail=1
    fi
else
    die_fail "\`catalog stat $IN_OBJ\` never answered through env attach in ${WAIT_VERB}s"
fi

if run_verb "catalog stat $OUT_OBJ" "refused by kernel.env.catalog"; then
    expect "CAT_STAT refused" "the foreign name comes back as the service's OWN refusal, visible in this transcript"
    reject "type="            "…and with no data — a refusal is not a shaped answer"
else
    die_fail "\`catalog stat $OUT_OBJ\` produced no refusal line in ${WAIT_VERB}s — a name outside the namespace must be answered by name, not left silent"
fi

# ─── 7. CAT_READ — the value that was written, and the key that was not ──
# The needle is the VALUE, which never appears in the line this guard typed
# into `catalog read`, so an echoed command line cannot satisfy it.
if run_verb "catalog read $IN_OBJ $REC_KEY" "$REC_VALUE"; then
    expect "$REC_VALUE" "CAT_READ returns the record field \`insert\` wrote, byte for byte"
else
    die_fail "\`catalog read $IN_OBJ $REC_KEY\` never returned '$REC_VALUE' in ${WAIT_VERB}s"
fi

if run_verb "catalog read $IN_OBJ nokey" "refused by kernel.env.catalog"; then
    expect "CAT_READ refused" "a key that does not exist is refused by name rather than answered with an empty value"
else
    die_fail "\`catalog read $IN_OBJ nokey\` produced no refusal line in ${WAIT_VERB}s"
fi

# ─── 8. CAT_QUERY — a real row-store table read through the boundary ────────
# Seeded the way every other namespace above is seeded: through the node's
# shell under the bearer uid, so valloc's partition_get_for_uid() lands the
# object INSIDE this partition and the teardown's partition destroy takes it
# with everything else. The value is unique to this run, so a page holding
# a row some earlier run inserted cannot satisfy the needle.
TBL="e91tbl_$RUN"
QVAL="e91q-$RUN"
WTBL="e91wtbl_$RUN"
# Text, not numeric: the SQL literal has to survive both the HTTP JSON
# decoder and the environment shell. The JSON body carries one escaped
# backslash (`\\`), which net/http.c decodes to the single backslash the
# shell reads as "quote me", so the line reaches the applet as
# `VALUES (\'text\')` and the native read-back can match the text exactly.
WVAL="e91w-$RUN"
WVAL2="e91w2-$RUN"

sh_exec "valloc $TBL 1 1" \
    || die_fail "'valloc $TBL' was not recognised by the node shell"
get /api/objects
if printf '%s' "$GET_BODY" | grep -Fq -- "\"$TBL\""; then
    echo "ok:   table object '$TBL' exists in the catalog (GET /api/objects names it)"
else
    die_fail "valloc never created '$TBL' — no table for CAT_QUERY to read (shell said: $(printf '%s' "$SHEL_OUT" | tail -2 | tr '\n' ' '))"
fi

sh_exec "schema set $TBL note STRING" \
    || die_fail "'schema set $TBL note STRING' was not recognised by the node shell"
sh_exec "table create $TBL" \
    || die_fail "'table create $TBL' was not recognised by the node shell"
case "$SHEL_OUT" in
    *"create '$TBL' -> OK"*)
        echo "ok:   \`table create $TBL\` promoted it to a row-set table (uses_rowstore — what CAT_QUERY requires)" ;;
    *)
        die_fail "'table create $TBL' did not promote the object (shell said: $(printf '%s' "$SHEL_OUT" | tail -3 | tr '\n' ' '))" ;;
esac

sh_exec "sql INSERT INTO $TBL (note) VALUES ('$QVAL')" \
    || die_fail "'sql INSERT INTO $TBL …' was not recognised by the node shell"
case "$SHEL_OUT" in
    *"[SQL] OK,"*)
        echo "ok:   the seed INSERT wrote one row carrying '$QVAL'" ;;
    *)
        die_fail "the seed INSERT never succeeded (shell said: $(printf '%s' "$SHEL_OUT" | tail -3 | tr '\n' ' ')) — nothing for CAT_QUERY to return" ;;
esac

# The page itself: the needle is the CELL VALUE, which never appears in the
# line this guard types — an echoed command or a plausible head cannot
# satisfy it — and the head is the service's own arithmetic over the real
# result set.
if run_verb "catalog sql SELECT * FROM $TBL" "$QVAL"; then
    expect "$QVAL" "CAT_QUERY returns the row the seed INSERT wrote, through the boundary, on this environment's console"
    expect "page 0: 1 row(s), 1 total" "the head reports exactly the one row the seed inserted (the cursor's own count)"
    expect "note" "the column names the service reported travel with the page"
    reject "refused" "the in-partition read is served, not refused"
else
    die_fail "\`catalog sql SELECT * FROM $TBL\` never returned '$QVAL' through env attach in ${WAIT_VERB}s — the SQL path is unreachable or the policy refused it (last output: ${VERB_OUT:-<nothing>})"
fi

# The read-only promise, live: DROP parses (it is a well-formed statement),
# then step 3 refuses it BY NAME. The row must still be there afterwards —
# a refusal that executed first would look identical in a transcript that
# only shows the refusal line.
if run_verb "catalog sql DROP TABLE $TBL" "CAT_QUERY refused by kernel.env.catalog"; then
    expect "only SELECT is served on this surface" "a DDL statement is refused BY NAME — the mediated surface admits SELECT alone"
    reject "row(s)" "…and with no page — a refusal is not a shaped answer"
else
    die_fail "\`catalog sql DROP TABLE $TBL\` produced no refusal line in ${WAIT_VERB}s — a non-SELECT reached the engine or was left silent"
fi

if run_verb "catalog sql SELECT * FROM $TBL" "$QVAL"; then
    expect "$QVAL" "the refused DROP never executed — the row is still there (deny-then-execute would have lost it)"
else
    die_fail "the table vanished after the refused DROP — execution happened behind a refusal (last output: ${VERB_OUT:-<nothing>})"
fi

# The boundary, on a name the SQL path resolved for itself: the foreign
# object exists in the SAME catalog but sits in PARTITION_SYSTEM, so
# cat_find() skips it and the answer is NOENT — not PERM (an existence
# oracle) and not rows (a boundary that leaked).
if run_verb "catalog sql SELECT * FROM $OUT_OBJ" "CAT_QUERY refused by kernel.env.catalog"; then
    reject "row(s)"             "no page — a foreign table yields nothing"
    reject "row-level policy denied" "the answer is absence (NOENT), not a PERM that names the object back"
    reject "$QVAL"               "certainly not this partition's data"
else
    die_fail "\`catalog sql SELECT * FROM $OUT_OBJ\` was not refused in ${WAIT_VERB}s — a foreign name was either served or left silent"
fi

# ─── 8b. M5.0 identity — a low-privilege environment must not run as uid 0 ──
# Before M5.0 every tenant sidecar copied init's boot owner (uid 0), so
# CAT_QUERY read dave's table as ROLE_SYSTEM_KERNEL from a low-privilege
# environment. This arm creates a SECOND environment in the same partition
# through the low-privilege shell, then (after APP_USER's global table-read
# grant would mask the bug) demotes its owner to ROLE_GUEST and requires
# dave's row to be refused by row-level policy.
echo "==> M5.0: low-privilege environment in partition $PART_ID"
peer_who="$(curl -s --max-time 15 -H "Authorization: Bearer $PEER_TOKEN" \
                  "http://127.0.0.1:$L_PORT/api/session/whoami" 2>/dev/null)"
PEER_UID="$(jget "$peer_who" uid)"
if [ "$PEER_UID" != "$PEER_UID_EXPECTED" ]; then
    die_fail "the low-privilege token did not authenticate as uid $PEER_UID_EXPECTED (whoami said: $peer_who)"
fi
echo "ok:   low-privilege token authenticates as uid $PEER_UID (ROLE_APP_USER)"

sh_exec_as "$PEER_TOKEN" "env create $PART_ID 2" \
    || die_fail "low-privilege shell did not recognise 'env create $PART_ID 2'"
case "$SHEL_OUT" in
    *"-> ok env_id="*)
        ENV2_ID="$(printf '%s' "$SHEL_OUT" | sed -n 's/.*env_id=\([0-9]*\).*/\1/p' | tail -1)" ;;
    *)
        die_fail "the low-privilege environment create did not succeed (shell said: $(printf '%s' "$SHEL_OUT" | tail -3 | tr '\n' ' '))" ;;
esac
if [ -z "$ENV2_ID" ] || [ "$ENV2_ID" = "0" ]; then
    die_fail "the low-privilege environment create returned no usable env_id (shell said: $(printf '%s' "$SHEL_OUT" | tail -3 | tr '\n' ' '))"
fi
echo "ok:   low-privilege peer created env $ENV2_ID in partition $PART_ID (index 2)"

# Wait for the guest environment's console, and tie it to the listing. The
# attach itself is done by the guest: the transcript that follows is the
# guest's own view, not dave's.
LIST2_ROW=""; PX2_PID=""
for i in $(seq 1 $((WAIT_ENVLIST * 2))); do
    sh_exec "env list $PART_ID" || true
    LIST2_ROW="$(printf '%s' "$SHEL_OUT" | grep -F "[ENV] env partition=$PART_ID env_id=$ENV2_ID " | head -1)"
    [ -n "$LIST2_ROW" ] && break
    sleep 0.5
done
if [ -z "$LIST2_ROW" ]; then
    die_fail "\`env list $PART_ID\` never reported low-privilege env $ENV2_ID (last output: ${SHEL_OUT:-<none>})"
fi
PX2_PID="$(printf '%s' "$LIST2_ROW" | grep -o 'posix_pid=[0-9]*' | cut -d= -f2)"
if [ -z "$PX2_PID" ] || [ "$PX2_PID" = "0" ]; then
    die_fail "the low-privilege environment's listing row carried no usable posix_pid (row: $LIST2_ROW)"
fi
echo "ok:   low-privilege env $ENV2_ID is listed with posix_pid=$PX2_PID"

# APP_USER is read-granted on DB_TABLE by catalog_role_grants(), so a bob
# environment would legitimately read dave's table even after M5.0. Demote bob
# to ROLE_GUEST for the assertion: a guessed/boot uid 0 still reads the row as
# ROLE_SYSTEM_KERNEL, while the bound uid 1001 is denied by row-level policy.
sh_exec "role set $PEER_UID_EXPECTED GUEST" >/dev/null 2>&1 \
    || die_fail "could not demote uid $PEER_UID_EXPECTED to ROLE_GUEST for the denial arm"
ATTACH_TOKEN="$TOKEN"
ATTACH_ENV_ID="$ENV2_ID"
ATTACH_OUT=""
announce2=""
for i in $(seq 1 $((WAIT_ENVLIST * 2))); do
    attach_drain
    announce2="$(printf '%s' "$ATTACH_OUT" | grep -o '\[env-id\] index=[0-9]* pid=[0-9]*' | head -1)"
    [ -n "$announce2" ] && break
    sleep 0.5
done
if [ "$announce2" = "[env-id] index=2 pid=$PX2_PID" ]; then
    echo "ok:   peer attach reaches the low-privilege environment's own console ('$announce2')"
else
    die_fail "the peer's console does not state this environment's identity (expected '[env-id] index=2 pid=$PX2_PID', got '${announce2:-nothing}')"
fi

# THE ASSERTION: dave's row-set table is in the same partition, but its
# owner is dave (uid $SEED_UID) and this environment's owner is bob
# (uid $PEER_UID, now ROLE_GUEST). A catalog channel bound to the inherited
# boot uid 0 is ROLE_SYSTEM_KERNEL and returns the row; a channel bound to
# the owner uid is refused by name, with no page and no value.
if run_verb "catalog sql SELECT * FROM $TBL" "CAT_QUERY refused by kernel.env.catalog"; then
    expect "row-level policy denied this uid" \
        "peer identity is bound to the catalog channel — dave's table is denied by row-level policy"
    reject "$QVAL" "the low-privilege environment did not receive dave's row (no uid-0 inheritance)"
    reject "row(s)" "the denial carried no result page"
else
    if printf '%s' "$VERB_OUT" | grep -Fq -- "$QVAL"; then
        die_fail "low-privilege env $ENV2_ID read dave's seed value '$QVAL' — the catalog channel is still authorizing as inherited boot uid 0"
    fi
    die_fail "low-privilege env $ENV2_ID did not receive CAT_QUERY's policy refusal in ${WAIT_VERB}s (last output: ${VERB_OUT:-<nothing>})"
fi
ATTACH_TOKEN=""
ATTACH_ENV_ID=""

# ─── 8c. M5.1 — the mediated write path, through bob's environment ────────
# Bob was demoted to GUEST for the read denial above; restore APP_USER so
# the write is decided by PERM_WRITE, not by the role's global read grant.
# The table is owned by bob and lives in the same partition, so the ONLY
# thing that lets this write through is the channel's bound owner.
echo "==> M5.1: mediated writes in partition $PART_ID"
sh_exec "role set $PEER_UID APP_USER" >/dev/null 2>&1 \
    || die_fail "could not restore uid $PEER_UID to ROLE_APP_USER for the write arm"

# Bob's valloc must resolve to THIS partition, or the table would land in
# PARTITION_SYSTEM and the environment would correctly answer NOENT.
post /api/partition/assign "{\"uid\":$PEER_UID,\"partition_id\":$PART_ID}"
if [ "$POST_STATUS" = "200" ] && [ "$(jget "$POST_BODY" ok)" = "true" ]; then
    echo "ok:   bob's uid $PEER_UID is assigned to partition $PART_ID"
else
    die_fail "could not assign bob's uid $PEER_UID to partition $PART_ID (HTTP $POST_STATUS: $POST_BODY)"
fi

sh_exec_as "$PEER_TOKEN" "valloc $WTBL 1 1" \
    || die_fail "low-privilege shell did not recognise 'valloc $WTBL'"
sh_exec_as "$PEER_TOKEN" "schema set $WTBL note STRING" \
    || die_fail "low-privilege shell did not recognise 'schema set $WTBL note STRING'"
sh_exec_as "$PEER_TOKEN" "table create $WTBL" \
    || die_fail "low-privilege shell did not recognise 'table create $WTBL'"
case "$SHEL_OUT" in
    *"create '$WTBL' -> OK"*)
        echo "ok:   bob ($PEER_UID) owns row-set table '$WTBL' in partition $PART_ID" ;;
    *)
        die_fail "'table create $WTBL' did not promote the object (shell said: $(printf '%s' "$SHEL_OUT" | tail -3 | tr '\n' ' '))" ;;
esac

# Attach to BOB's environment, still over the bearer's console transport: the
# commands run in the environment, so the kernel binds them to bob's owner.
ATTACH_TOKEN="$TOKEN"
ATTACH_ENV_ID="$ENV2_ID"
ATTACH_OUT=""

if run_verb "catalog exec INSERT INTO $WTBL (note) VALUES (\\'$WVAL\\')" "row(s) affected"; then
    expect "1 row(s) affected" "CAT_EXEC INSERT succeeds for the channel's bound owner — the commit is acknowledged"
    reject "refused" "the owner's own table is not refused"
else
    die_fail "\`catalog exec INSERT INTO $WTBL ...\` never answered through env attach in ${WAIT_VERB}s (last output: ${VERB_OUT:-<nothing>})"
fi

# The independent observer: dave's native SQL path, outside the mediated
# service, must see the row. A CAT_EXEC that answered OK before its commit
# would leave this read empty. The needle never appears in the command line.
sh_exec "sql SELECT * FROM $WTBL" \
    || die_fail "the native shell did not recognise 'sql SELECT * FROM $WTBL'"
if printf '%s' "$SHEL_OUT" | grep -Fq -- "$WVAL"; then
    echo "ok:   native SQL observes bob's committed row '$WVAL' — CAT_OK meant committed"
else
    die_fail "native SQL did not see '$WVAL' after CAT_EXEC reported success (shell said: $(printf '%s' "$SHEL_OUT" | tail -4 | tr '\n' ' '))"
fi

# UPDATE and DELETE go through the same accepted statement and commit path;
# the native observer must see each committed effect in turn.
if run_verb "catalog exec UPDATE $WTBL SET note = \\'$WVAL2\\'" "row(s) affected"; then
    expect "1 row(s) affected" "CAT_EXEC UPDATE commits against the owner's table"
else
    die_fail "\`catalog exec UPDATE $WTBL ...\` never answered through env attach in ${WAIT_VERB}s (last output: ${VERB_OUT:-<nothing>})"
fi

sh_exec "sql SELECT * FROM $WTBL" \
    || die_fail "the native shell did not recognise 'sql SELECT * FROM $WTBL' after the update"
if printf '%s' "$SHEL_OUT" | grep -Fq -- "$WVAL2"; then
    echo "ok:   native SQL observes the committed UPDATE value '$WVAL2'"
else
    die_fail "native SQL did not see '$WVAL2' after the committed CAT_EXEC UPDATE (shell said: $(printf '%s' "$SHEL_OUT" | tail -4 | tr '\n' ' '))"
fi

if run_verb "catalog exec DELETE FROM $WTBL" "row(s) affected"; then
    expect "1 row(s) affected" "CAT_EXEC DELETE commits against the owner's table"
else
    die_fail "\`catalog exec DELETE FROM $WTBL\` never answered through env attach in ${WAIT_VERB}s (last output: ${VERB_OUT:-<nothing>})"
fi

sh_exec "sql SELECT * FROM $WTBL" \
    || die_fail "the native shell did not recognise 'sql SELECT * FROM $WTBL' after the delete"
if printf '%s' "$SHEL_OUT" | grep -Fq -- "$WVAL2"; then
    die_fail "native SQL still sees '$WVAL2' after the committed CAT_EXEC DELETE (shell said: $(printf '%s' "$SHEL_OUT" | tail -4 | tr '\n' ' '))"
else
    echo "ok:   native SQL no longer sees '$WVAL2' — the DELETE committed"
fi

# The catalog's legacy default perm_mask carries WRITE, so APP_USER bob would
# otherwise pass the object-level fallback on dave's table. Pin that table to
# PERM_READ only: the next DELETE is then decided by the PERM_WRITE gate. The
# native read proves the row was not deleted behind the refusal.
post /api/chmod "{\"name\":\"$TBL\",\"mask\":\"0x4\"}"
if [ "$POST_STATUS" = "200" ] && [ "$(jget "$POST_BODY" ok)" = "true" ]; then
    echo "ok:   dave's table is read-only (perm_mask=0x4), so PERM_WRITE is the deciding gate"
else
    die_fail "could not pin dave's table to PERM_READ (HTTP $POST_STATUS: $POST_BODY)"
fi

# The same environment, the same verb, dave's read-only table: bob is APP_USER
# and not the owner, so PERM_WRITE is refused BY NAME.
if run_verb "catalog exec DELETE FROM $TBL" "CAT_EXEC refused by kernel.env.catalog"; then
    expect "PERM_WRITE" "a non-owner write is refused PERM_WRITE by name"
    reject "row(s) affected" "a refusal carries no affected-row body"
else
    die_fail "\`catalog exec DELETE FROM $TBL\` produced no refusal in ${WAIT_VERB}s — a non-owner write reached the engine or was left silent"
fi

sh_exec "sql SELECT * FROM $TBL" \
    || die_fail "the native shell did not recognise 'sql SELECT * FROM $TBL' after the refused write"
if printf '%s' "$SHEL_OUT" | grep -Fq -- "$QVAL"; then
    echo "ok:   dave's row '$QVAL' is still there — the refused DELETE did not execute"
else
    die_fail "dave's row '$QVAL' vanished after a refused CAT_EXEC DELETE (shell said: $(printf '%s' "$SHEL_OUT" | tail -4 | tr '\n' ' '))"
fi

# The write surface is not a DDL path: the statement parses, then MEANING
# refuses it by name before any lookup or transaction.
if run_verb "catalog exec DROP TABLE $WTBL" "CAT_EXEC refused by kernel.env.catalog"; then
    expect "only INSERT, UPDATE or DELETE" "a DDL statement is refused by name — CAT_EXEC is not ALTER"
    reject "row(s) affected" "no affected-row body is sent with the refusal"
else
    die_fail "\`catalog exec DROP TABLE $WTBL\` produced no TYPE refusal in ${WAIT_VERB}s"
fi

# The boundary, on the write path: the foreign object exists in the same
# catalog, in PARTITION_SYSTEM, so the answer is NOENT — the same absence a
# name that never existed gets, not a PERM that confirms it.
if run_verb "catalog exec INSERT INTO $OUT_OBJ (k) VALUES (1)" "CAT_EXEC refused by kernel.env.catalog"; then
    reject "PERM_WRITE" "a foreign write target is absence, not a PERM that confirms it"
    reject "not a row-set table" "the foreign object was never resolved far enough to describe its kind"
    reject "row(s) affected" "a foreign write produced no affected-row body"
else
    die_fail "\`catalog exec INSERT INTO $OUT_OBJ ...\` produced no refusal in ${WAIT_VERB}s"
fi

ATTACH_TOKEN=""
ATTACH_ENV_ID=""

# ─── 9. teardown (the EXIT trap runs it from here either way) ─────────────
[ "$fail" -eq 0 ] || report_and_exit

echo "OK: §9.1's catalog surface answered end to end — a partition and an environment created in it, info/list/stat/read/sql driven into that environment through \`env attach\`, every answer (all refusals included) came back on the environment's own console, and M5.1's CAT_EXEC committed the owner's write while refusing the non-owner's"
exit 0
