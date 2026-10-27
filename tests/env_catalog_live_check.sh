#!/usr/bin/env bash
# tests/env_catalog_live_check.sh — §9.1's mediated catalog, exercised for
# real: on a RUNNING cluster, define a partition, seed its namespace with an
# object (and one object deliberately outside it), create an environment in
# that partition, and then drive `catalog info | list | stat | read` INTO that
# environment through `env attach` — requiring every answer to come back on
# the environment's own console.
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
PART_ID=""
OUT_OBJ_CREATED=""
VERB_OUT=""
TEARDONE=0

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

# ─── env attach: send a line, drain the answer ────────────────────────────
# `env attach <partition> <env> <line>` writes into the environment's OWN
# console (the kernel appends the terminator — env_console.c: a line needs
# its terminator or the sh applet never completes the command); a bare
# `env attach …` drains what the environment produced. env_console_write
# refuses a line while the peer still holds the previous one (the kernel's
# flow control), so the send retries exactly as
# tests/env_shell_surface_check.sh's console_send does.
ATTACH_OUT=""
attach_drain() {
    sh_exec "env attach $PART_ID $ENV_ID" || true
    ATTACH_OUT="$ATTACH_OUT$SHEL_OUT"
}

attach_send() {   # $1 = the line
    local t
    for t in $(seq 1 20); do
        sh_exec "env attach $PART_ID $ENV_ID $1" || true
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

# ─── 8. teardown (the EXIT trap runs it from here either way) ─────────────
[ "$fail" -eq 0 ] || report_and_exit

echo "OK: §9.1's catalog surface answered end to end — a partition and an environment created in it, info/list/stat/read driven into that environment through \`env attach\`, and every answer (both refusals included) came back on the environment's own console"
exit 0
