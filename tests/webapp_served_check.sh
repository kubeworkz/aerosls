#!/usr/bin/env bash
# tests/webapp_served_check.sh — a LIVE kernel instance must serve exactly
# the committed webapp bundle, byte for byte.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/webapp_bundle.c is canonical: CI and the deploy build the kernel
# from the committed file, and tests/webapp_bundle_guard_check.sh proves the
# committed file matches the recorded slsos-sim revision. But "the build
# embedded the right bytes" and "the running kernel serves those bytes" are
# different properties. The webapp is served by the kernel's own HTTP server
# through bundle_find()/_bundle_table — a serving bug (wrong path mapping, a
# truncated response, a re-encoded asset) would ship a broken Navigator SPA
# with every guard above green. The 2026-08-18 incident was precisely a
# served webapp that existed nowhere in the repo; this guard closes the loop
# on the SERVED side: every asset a deployed instance serves must be
# byte-identical to the committed bundle it was built from.
#
# ─── Where it runs ─────────────────────────────────────────────────────────
# Authoritative call: deploy.sh, AFTER the pm2 restart and the relational
# health wait — it verifies the NEW kernel. It also participates in the
# tests/run_checks.sh gate: against whatever instance is live then (on the
# server, the pre-restart kernel — which must serve the same committed
# bundle), and as an owed skip on a build host that has no live instance at
# all. A build host can no more conjure a running kernel than a running
# cluster, which is why this carries the same GUARD-KIND: runtime marker as
# the cluster guards.
#
# GUARD-KIND: runtime (needs a LIVE deployed kernel instance, never a build)
#
# Usage:
#   tests/webapp_served_check.sh                            # $SERVED_URL or http://localhost:3001
#   tests/webapp_served_check.sh --url http://host:port
#   tests/webapp_served_check.sh --bundle /path/to/webapp_bundle.c   # smoke override
#
# Exit: 0 pass, 1 fail (a served asset drifts from the committed bundle),
#       2 abort (no live instance / missing prerequisite).
set -u
cd "$(dirname "$0")/.."   # repo root

URL="${SERVED_URL:-http://localhost:3001}"
BUNDLE="kernel/webapp_bundle.c"
while [ "$#" -gt 0 ]; do
    case "$1" in
        --url)    URL="${2:?--url needs a value}"; shift 2 ;;
        --bundle) BUNDLE="${2:?--bundle needs a value}"; shift 2 ;;
        -h|--help) sed -n '2,/^set -u/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "error: unknown option '$1'" >&2; exit 2 ;;
    esac
done
URL="${URL%/}"

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
[ -f "$BUNDLE" ] || { echo "ABORT: $BUNDLE not found" >&2; exit 2; }

# A live instance must be answering before anything is compared. On a build
# host there is none — that is a prerequisite, not a finding (GUARD-KIND:
# runtime, so run_checks.sh counts it as an owed skip there).
if ! curl -sf --max-time 5 "$URL/" >/dev/null 2>&1; then
    echo "ABORT: no live kernel instance answering $URL/ — this guard needs a" >&2
    echo "       deployed kernel (post-restart on the server), not a build." >&2
    exit 2
fi

python3 - "$BUNDLE" "$URL" <<'PY'
import re, sys, urllib.request

bundle_path, base = sys.argv[1], sys.argv[2]
src = open(bundle_path).read()

# Parse the embedded arrays and the bundle table out of the committed file.
arrays = {}
for m in re.finditer(r'static const uint8_t (_bundle_\w+)\[\] = \{(.*?)\};', src, re.S):
    arrays[m.group(1)] = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(2)))

table = []
for m in re.finditer(r'\{\s*"(/[^"]*)",\s*"([^"]*)",\s*(_bundle_\w+),\s*(\d+)U?\s*\}', src):
    table.append((m.group(1), m.group(2), m.group(3), int(m.group(4))))

if not arrays or not table:
    print("FAIL  could not parse embedded arrays/table from %s" % bundle_path)
    sys.exit(1)

def fetch(path):
    req = urllib.request.Request(base + path,
                                 headers={'User-Agent': 'webapp-served-check'})
    with urllib.request.urlopen(req, timeout=10) as r:
        return r.read()

ok = True
for path, mime, ident, length in table:
    emb = arrays.get(ident)
    if emb is None or len(emb) != length:
        print("FAIL  %-30s embedded array %s missing or length mismatch" % (path, ident))
        ok = False
        continue
    try:
        served = fetch(path)
    except Exception as e:
        print("FAIL  %-30s fetch failed: %s" % (path, e))
        ok = False
        continue
    same = emb == served
    ok = ok and same
    print("%s  %-30s %8d served vs %8d embedded%s"
          % ("OK" if same else "FAIL", path, len(served), len(emb),
             "" if same else "  -- DRIFT"))

if not ok:
    print("webapp_served_check: FAILED -- the served webapp drifts from the committed bundle")
    sys.exit(1)
print("webapp_served_check: done, all %d assets byte-identical to %s" % (len(table), bundle_path))
PY
