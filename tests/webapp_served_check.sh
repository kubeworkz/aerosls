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
# byte-identical to the committed bundle it was built from, AND the response
# must carry the right headers -- Content-Type equal to the table's MIME and
# Content-Length equal to the embedded length. The kernel's http_respond_raw
# emits exactly those (net/http.c), so a serving path that returns the right
# bytes under wrong headers is still drift.
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
import http.client, re, sys
from urllib.parse import urlsplit

bundle_path, base = sys.argv[1], sys.argv[2]
parts = urlsplit(base)
host, port = parts.hostname, parts.port or 80

# Parse the embedded arrays and the bundle table out of the committed file.
src = open(bundle_path).read()
arrays = {}
for m in re.finditer(r'static const uint8_t (_bundle_\w+)\[\] = \{(.*?)\};', src, re.S):
    arrays[m.group(1)] = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})', m.group(2)))

table = []
for m in re.finditer(r'\{\s*"(/[^"]*)",\s*"([^"]*)",\s*(_bundle_\w+),\s*(\d+)U?\s*\}', src):
    table.append((m.group(1), m.group(2), m.group(3), int(m.group(4))))

if not arrays or not table:
    print("FAIL  could not parse embedded arrays/table from %s" % bundle_path)
    sys.exit(1)

# A fresh connection per asset (the kernel does not keep-alive, and a wrong
# Content-Length can leave bytes stranded in the socket). http.client, not
# urllib, so the raw headers the kernel emitted are visible for assertion.
def fetch(path):
    conn = http.client.HTTPConnection(host, port, timeout=10)
    try:
        conn.request('GET', path, headers={'User-Agent': 'webapp-served-check'})
        resp = conn.getresponse()
        return (resp.status,
                resp.getheader('Content-Type'),
                resp.getheader('Content-Length'),
                resp.read())
    finally:
        conn.close()

ok = True
for path, mime, ident, length in table:
    emb = arrays.get(ident)
    if emb is None or len(emb) != length:
        print("FAIL  %-30s embedded array %s missing or length mismatch" % (path, ident))
        ok = False
        continue
    try:
        status, ctype, clen, served = fetch(path)
    except Exception as e:
        print("FAIL  %-30s fetch failed: %s" % (path, e))
        ok = False
        continue
    bad = []
    if status != 200:
        bad.append("status %d, expected 200" % status)
    if ctype != mime:
        bad.append("Content-Type '%s', expected '%s'" % (ctype, mime))
    try:
        cl_ok = clen is not None and int(clen) == len(emb)
    except ValueError:
        cl_ok = False
    if not cl_ok:
        bad.append("Content-Length '%s', expected %d" % (clen, len(emb)))
    if served != emb:
        bad.append("%d served vs %d embedded bytes differ" % (len(served), len(emb)))
    if bad:
        ok = False
        print("FAIL  %-30s %s" % (path, "; ".join(bad)))
    else:
        print("OK    %-30s %8d bytes  %s  CL %s" % (path, len(served), ctype, clen))

if not ok:
    print("webapp_served_check: FAILED -- the served webapp drifts from the committed bundle")
    sys.exit(1)
print("webapp_served_check: done, all %d assets byte-identical (body + headers) to %s"
      % (len(table), bundle_path))
PY
