#!/usr/bin/env bash
# tests/entropy_boot_diversity_smoke.sh — the diversity guard can FAIL.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# A guard that only ever passes proves nothing, and this repo already learned
# that the expensive way twice in one week:
#
#   - script_conventions_check silently stopped inspecting any files at all
#     when a pathspec was written with `-z` after `--`. It reported "0
#     violations" in green. Two of its three teeth stopped biting, and the
#     smoke is the only reason anybody found out.
#
#   - entropy_boot_diversity_check itself passed nothing for days because the
#     kernel never called entropy_init(). Every host test passed throughout,
#     because host tests call it themselves.
#
# So this plants teeth. It stands up fake nodes on a spare port base and
# requires the guard to reach the right verdict for each shape, including the
# ones that must NOT be reported as diversity failures.
#
#   distinct fingerprints  -> PASS   (exit 0)
#   identical fingerprints -> FAIL   (exit 1) -- the Debian shape
#   ready=false            -> FAIL   (exit 1) naming fail-closed, NOT diversity
#   HTTP 401               -> ABORT  (exit 2) naming the token
#   nothing listening      -> ABORT  (exit 2) naming a refused connect
#
# The last three matter as much as the second. A guard that shouts "seeding is
# not varying" at a node which simply has no entropy, or at a missing bearer
# token, sends the reader hunting a cryptographic defect that is not there --
# which this script's own history demonstrates, twice.
#
# Uses a port base far from any real cluster so it cannot be confused with one,
# and never touches the git index.
#
# Exit: 0 all teeth bit, 1 a tooth failed, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

GUARD=tests/entropy_boot_diversity_check.sh
BASE=58900

[ -x "$GUARD" ] || { echo "ABORT: $GUARD not found or not executable."; exit 2; }
command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 needed to stand up fake nodes."; exit 2; }
command -v curl    >/dev/null 2>&1 || { echo "ABORT: curl not found."; exit 2; }

fails=0
SERVER_PID=""

cleanup() {
    [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
    wait "$SERVER_PID" 2>/dev/null
    return 0
}
trap cleanup EXIT

# $1 = mode, $2 = expected exit, $3 = a phrase the output must contain, $4 = label
tooth() {
    local mode="$1" want_rc="$2" want_txt="$3" label="$4"
    SERVER_PID=""

    if [ "$mode" != "silent" ]; then
        # stdout/stderr to /dev/null: a background process inheriting this
        # script's stdout keeps the write end of any pipe open, so `... | tail`
        # would hang after a full pass. That exact bug cost a debugging round
        # in tests/run_cluster_stop_check.sh.
        python3 tests/entropy_boot_diversity_smoke_nodes.py "$mode" "$BASE" >/dev/null 2>&1 &
        SERVER_PID=$!
        # Wait for the first port to answer rather than sleeping a guess.
        for _ in $(seq 1 40); do
            curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/entropy" && break
            sleep 0.1
        done
    fi

    out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_NODES=3 \
           AEROSLS_TOKEN=smoke-token bash "$GUARD" 2>&1)"
    rc=$?
    cleanup

    if [ "$rc" != "$want_rc" ]; then
        echo "TOOTH FAIL $label — guard exit $rc, expected $want_rc"
        printf '%s\n' "$out" | sed 's/^/           /'
        fails=$((fails + 1))
        return
    fi
    case "$out" in
        *"$want_txt"*) echo "TOOTH OK   $label — exit $rc, said: $want_txt" ;;
        *)
            echo "TOOTH FAIL $label — exit $rc was right, but the message did not mention: $want_txt"
            printf '%s\n' "$out" | sed 's/^/           /'
            fails=$((fails + 1)) ;;
    esac
}

tooth distinct  0 "every node seeded differently"        "distinct -> PASS"
tooth identical 1 "distinct fingerprints"                "identical -> FAIL (the Debian shape)"
tooth unseeded  1 "NOT SEEDED"                           "ready=false -> FAIL, named as fail-closed"
tooth denied    2 "rejected the request"                 "401 -> ABORT naming the token"
tooth silent    2 "connection refused"                   "nothing listening -> ABORT naming the connect"

# The unseeded tooth must NOT be reported as a diversity problem. Checking the
# verdict alone would pass even if the wording blamed the wrong thing, which is
# the failure this guard has actually shipped twice.
SERVER_PID=""
python3 tests/entropy_boot_diversity_smoke_nodes.py unseeded "$BASE" >/dev/null 2>&1 &
SERVER_PID=$!
for _ in $(seq 1 40); do
    curl -s --max-time 1 -o /dev/null "http://127.0.0.1:$((BASE+1))/api/entropy" && break
    sleep 0.1
done
out="$(AEROSLS_HTTP_BASE=$BASE AEROSLS_NODES=3 AEROSLS_TOKEN=smoke-token bash "$GUARD" 2>&1)"
cleanup
case "$out" in
    *"predictable to anyone"*)
        echo "TOOTH FAIL unseeded is not blamed on diversity — it used the seeding-diversity wording"
        fails=$((fails + 1)) ;;
    *)  echo "TOOTH OK   unseeded is not blamed on diversity" ;;
esac

echo
if [ "$fails" -eq 0 ]; then
    echo "entropy_boot_diversity_smoke: done, 0 teeth failed"
    exit 0
fi
echo "entropy_boot_diversity_smoke: done, $fails teeth failed"
exit 1
