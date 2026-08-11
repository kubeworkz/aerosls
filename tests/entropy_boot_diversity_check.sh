#!/usr/bin/env bash
# tests/entropy_boot_diversity_check.sh — two nodes booted from one image must
# not produce the same randomness.
#
# ─── Why this is the gate that matters ─────────────────────────────────────
# Everything else in the entropy test suite runs inside one process and can
# only check that the DRBG is the algorithm it claims to be and that it fails
# safely. None of it can detect the failure that actually ships: seeding that
# does not vary. A DRBG seeded from a constant passes every known-answer test,
# every health test, every statistical test, and produces beautiful output --
# the same beautiful output on every machine.
#
# That is not a hypothetical. Debian's OpenSSL shipped it for two years, and
# the reason nobody noticed is that there was no test of this shape anywhere.
# One comparison across two machines would have caught it on day one.
#
# ─── What it compares ──────────────────────────────────────────────────────
# GET /api/entropy returns boot_fingerprint: SHA-256 of a domain-separated
# label and 32 bytes generated once at seeding, which were then destroyed. It
# is one-way and those bytes are used for nothing else, so publishing it costs
# nothing -- see kernel/entropy.h. Comparing raw DRBG output would have been
# easier to write and would have handed out part of the generator's stream.
#
# The nodes must be booted from the SAME kernel image for this to mean
# anything. run-cluster.sh builds one ISO per node (they differ only in the
# node= command line), which is the condition this test wants: identical
# entropy code, identical build, nothing different but the hardware moment.
#
# ─── Usage ─────────────────────────────────────────────────────────────────
#   ./run-cluster.sh --nodes 3          # in another shell
#   tests/entropy_boot_diversity_check.sh
#
# Environment: AEROSLS_HTTP_BASE (default 3000), AEROSLS_NODES (default: probe
# until a port does not answer), AEROSLS_TOKEN (bearer token for /api/*).
#
# Exit: 0 pass, 1 fail, 2 skip (no cluster reachable, or no token).
#
# GUARD-KIND: runtime
#
# That marker is read by tests/run_checks.sh and it matters. Every other guard
# here inspects the SOURCE or a BUILD ARTEFACT, so on a build host its
# prerequisite exists and --require-all is right to treat a skip as a failure.
# This one needs a RUNNING CLUSTER, which deploy.sh cannot have: it runs the
# guards against a freshly built image and has not restarted the kernel yet.
# Under --require-all it therefore failed every deploy, unconditionally, and
# blocked shipping for a reason that had nothing to do with the build.
#
# A guard that can never pass where it is run is not a strict gate, it is a
# broken one -- and the pressure it creates is to pass --no-verify, which
# disables the twelve guards that WERE meaningful. So this is skipped there and
# reported as owed, loudly, rather than turned into noise everyone learns to
# route around.
set -u
cd "$(dirname "$0")/.."

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
MAX_NODES="${AEROSLS_NODES:-8}"

command -v curl >/dev/null 2>&1 || { echo "ABORT: curl not found."; exit 2; }

# Returns "<http_status> <body>". Deliberately NOT curl -f: -f makes curl exit
# non-zero on 401/404 with no output, and the caller then cannot tell "nothing
# is listening" from "the node answered, and said no".
#
# That distinction is the entire point of this function. The first version of
# this script used -f, and when a running cluster returned 401 -- because
# /api/entropy sits behind the same bearer-token gate as every other /api route
# -- it reported "no node answered ... start a cluster first" to somebody who
# had just started one. A diagnostic that names the wrong cause is worse than
# no diagnostic: it sends the reader to fix something that was never broken.
# Sets FETCH_STATUS / FETCH_RC / FETCH_BODY rather than returning a string,
# because curl's EXIT CODE is the diagnosis whenever there is no HTTP status
# and discarding it is how this script ended up telling a running cluster to
# start itself.
#
# Status 000 does not mean "nothing is listening". On this deployment the
# ports are QEMU hostfwd: QEMU accepts the TCP connection itself and forwards
# into the guest over slirp, so connect() ALWAYS succeeds whether or not the
# guest kernel is answering. curl then reports 000 for a timeout, an empty
# reply, and a refused connection alike -- three different faults with three
# different fixes. The exit code separates them.
FETCH_STATUS=""; FETCH_RC=0; FETCH_BODY=""
fetch() {   # $1 = port
    local out
    if [ -n "$TOKEN" ]; then
        out="$(curl -s --max-time 8 -w '\n%{http_code}' \
                    -H "Authorization: Bearer $TOKEN" \
                    "http://127.0.0.1:$1/api/entropy" 2>/dev/null)"
    else
        out="$(curl -s --max-time 8 -w '\n%{http_code}' \
                    "http://127.0.0.1:$1/api/entropy" 2>/dev/null)"
    fi
    FETCH_RC=$?
    FETCH_STATUS="$(printf '%s' "$out" | tail -n1)"
    FETCH_BODY="$(printf '%s' "$out" | sed '$d')"
}

# curl exit codes, named. Guessing from 000 alone is what this replaces.
curl_reason() {
    case "$1" in
        7)  echo "connection refused -- nothing accepted the TCP connect" ;;
        28) echo "TIMED OUT -- the connect succeeded but no HTTP response arrived. On a QEMU hostfwd port this means QEMU accepted and the GUEST did not answer" ;;
        52) echo "empty reply -- the server accepted, then closed without sending anything" ;;
        56) echo "receive failure -- the connection broke mid-response" ;;
        6)  echo "could not resolve host" ;;
        0)  echo "curl reported success but no HTTP status was parsed" ;;
        *)  echo "curl exit $1" ;;
    esac
}

# Extract a JSON string field without a JSON parser. The kernel emits flat
# objects with no nesting and no escapes in these fields, so this is adequate
# here and would not be in general.
# Optional whitespace after the colon is tolerated. The kernel's jb_str emits
# none, but a test double or a future proxy may, and a field-extractor that
# silently returns empty on a space would report every node as unseeded --
# which is what it did on first run here.
jfield() { printf '%s' "$1" | sed -n "s/.*\"$2\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p"; }

fps=""
ids=""
found=0
unseeded=""

needs_token=""
route_missing=""
other_status=""
silent=""

for i in $(seq 1 "$MAX_NODES"); do
    port=$((HTTP_BASE + i))
    fetch "$port"
    status="$FETCH_STATUS"; body="$FETCH_BODY"

    case "$status" in
        000|"")
            silent="${silent}node $i (port $port): $(curl_reason "$FETCH_RC")"$'\n'
            continue ;;
        200) ;;                                   # fall through and parse
        401|403)
            needs_token="${needs_token}node $i (port $port): HTTP $status"$'\n'
            found=$((found + 1)); continue ;;
        404)
            route_missing="${route_missing}node $i (port $port): HTTP 404"$'\n'
            found=$((found + 1)); continue ;;
        *)
            other_status="${other_status}node $i (port $port): HTTP $status"$'\n'
            found=$((found + 1)); continue ;;
    esac
    found=$((found + 1))

    ready="$(jfield "$body" ready)"
    fp="$(jfield "$body" boot_fingerprint)"

    if [ "$ready" != "true" ] || [ -z "$fp" ]; then
        # A node with no entropy is a different failure from two nodes with the
        # SAME entropy, and conflating them would send someone looking for a
        # seeding-diversity bug when the actual problem is no seeding at all.
        unseeded="${unseeded}node $i (port $port): ready=$ready"$'\n'
        continue
    fi

    ids="${ids}$i "
    fps="${fps}$fp $i"$'\n'
done

if [ -n "$needs_token" ]; then
    echo "ABORT: node(s) are UP and answered, but rejected the request:"
    printf '%s' "$needs_token" | sed 's/^/        /'
    echo
    echo "       /api/entropy sits behind the same bearer-token gate as every"
    echo "       other /api route -- only /api/health is exempt. The cluster is"
    echo "       fine; this script has no credentials."
    echo
    echo "       Set a token and re-run:"
    echo "           AEROSLS_TOKEN=<token> tests/entropy_boot_diversity_check.sh"
    echo
    echo "       The four demo tokens are compiled in at kernel/auth.c:94 and"
    echo "       printed on each node's console at boot ([AUTH] Token Registry)."
    echo "       dave@gridworkz.com is ROLE_DB_ADMIN and will do:"
    echo
    echo "           AEROSLS_TOKEN=deadbeef01234567cafebabe76543210 \\"
    echo "               tests/entropy_boot_diversity_check.sh"
    echo
    echo "       Naming it here costs nothing -- it is a fixed constant in the"
    echo "       source and is printed in the clear at every boot. That is the"
    echo "       point worth noticing: these four tokens never expire"
    echo "       (no_expiry=1) and two of them are DB_ADMIN. Fine for a local"
    echo "       simulator, not fine on a node a real user touches."
    exit 2
fi

if [ -n "$route_missing" ]; then
    echo "ABORT: node(s) are UP but have no /api/entropy route:"
    printf '%s' "$route_missing" | sed 's/^/        /'
    echo
    echo "       That route was added with the boot fingerprint. A 404 means the"
    echo "       RUNNING kernel predates it -- most likely a deploy aborted at a"
    echo "       guard and left the previous build running, which deploy.sh says"
    echo "       explicitly when it happens. Rebuild and restart, then re-run."
    exit 2
fi

if [ -n "$other_status" ]; then
    echo "ABORT: node(s) answered with an unexpected status:"
    printf '%s' "$other_status" | sed 's/^/        /'
    exit 2
fi

if [ "$found" -eq 0 ]; then
    echo "ABORT: no node produced an HTTP response on ports"
    echo "       $((HTTP_BASE+1))..$((HTTP_BASE+MAX_NODES)). Per port:"
    echo
    printf '%s' "$silent" | sed 's/^/        /'
    echo
    echo "       Read the reason before assuming the cluster is down. If it says"
    echo "       TIMED OUT, the port IS bound -- check with 'netstat -tulpn' --"
    echo "       and the guest kernel is not answering that request. If it says"
    echo "       connection refused, nothing is there and a cluster is needed:"
    echo "           ./run-cluster.sh --nodes 3"
    echo "       A different port base is set with AEROSLS_HTTP_BASE."
    exit 2
fi

if [ -n "$unseeded" ]; then
    echo "FAIL  node(s) reachable but NOT SEEDED -- entropy_init() refused:"
    printf '%s' "$unseeded" | sed 's/^/        /'
    echo "        This is fail-closed working as designed, not a diversity"
    echo "        problem. The node has no usable entropy source; check the"
    echo "        [ENTROPY] lines on its console before reading anything else"
    echo "        into this."
    exit 1
fi

n="$(printf '%s' "$fps" | grep -c .)"
if [ "$n" -lt 2 ]; then
    echo "ABORT: only $n seeded node(s) found. Diversity needs at least two."
    echo "       Start more:  ./run-cluster.sh --nodes 3"
    exit 2
fi

# The whole test, in one line: how many DISTINCT fingerprints among n nodes.
distinct="$(printf '%s' "$fps" | awk '{print $1}' | sort -u | grep -c .)"

echo "  nodes seeded:        $n  (ids: $ids)"
echo "  distinct fingerprints: $distinct"

if [ "$distinct" -eq "$n" ]; then
    echo "PASS  every node seeded differently ($n/$n distinct)"
    exit 0
fi

echo
echo "FAIL  $n nodes produced only $distinct distinct fingerprints."
echo
printf '%s' "$fps" | awk '{print "        node " $2 ": " substr($1,1,32) "..."}'
echo
echo "        Two nodes from one image generating identical randomness means"
echo "        seeding is not varying. Every key this cluster generates is"
echo "        predictable to anyone who can boot the same image."
echo
echo "        This is the Debian OpenSSL failure. Do not ship, do not work"
echo "        around it in the caller, and do not treat a passing"
echo "        entropy_host_test as evidence against it -- that test runs in"
echo "        one process and cannot see this."
exit 1
