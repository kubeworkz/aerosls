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
set -u
cd "$(dirname "$0")/.."

HTTP_BASE="${AEROSLS_HTTP_BASE:-3000}"
TOKEN="${AEROSLS_TOKEN:-}"
MAX_NODES="${AEROSLS_NODES:-8}"

command -v curl >/dev/null 2>&1 || { echo "ABORT: curl not found."; exit 2; }

fetch() {   # $1 = port
    if [ -n "$TOKEN" ]; then
        curl -fsS --max-time 5 -H "Authorization: Bearer $TOKEN" \
             "http://127.0.0.1:$1/api/entropy" 2>/dev/null
    else
        curl -fsS --max-time 5 "http://127.0.0.1:$1/api/entropy" 2>/dev/null
    fi
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

for i in $(seq 1 "$MAX_NODES"); do
    port=$((HTTP_BASE + i))
    body="$(fetch "$port")" || continue
    [ -z "$body" ] && continue
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

if [ "$found" -eq 0 ]; then
    echo "ABORT: no node answered /api/entropy on ports $((HTTP_BASE+1))..$((HTTP_BASE+MAX_NODES))."
    echo "       Start a cluster first:  ./run-cluster.sh --nodes 3"
    echo "       If /api/* needs a token, set AEROSLS_TOKEN."
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
