#!/usr/bin/env bash
# tests/webapp_served_smoke.sh — proves tests/webapp_served_check.sh's teeth
# bite, without a kernel: a local python http.server serves a MINIATURE
# bundle, the guard is pointed at it via --url/--bundle, and planted drift
# must flip the verdict PASS -> FAIL -> FAIL.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The served check compares what a live instance SERVES against the bytes
# EMBEDDED in the committed bundle. A guard that passes while inspecting
# nothing is rot, and this one has two natural blind spots: (1) a guard
# wired only into deploy.sh's post-restart step could pass review and never
# run anywhere testable; (2) the compare logic (C-array parsing, HTTP fetch,
# byte equality) could regress in a way that only a live kernel would
# expose. This smoke exercises the exact logic against a controllable
# server: matching content must PASS, drifted content must FAIL, a missing
# asset must FAIL. It is source-only (bash + python3, no build, no kernel),
# so run_source_smokes.sh covers it on every push.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh and deploy.sh gates on that same set.
# This script starts its own HTTP server and writes to a temp dir; it must
# never run in the deploy gate. Run it with the other source smokes.
set -u
cd "$(dirname "$0")/.."   # repo root

GUARD=tests/webapp_served_check.sh
TMP="$(mktemp -d)"
PORT=""
SERVER_PID=""
cleaned=0
pass=0; fail=0

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found" >&2; exit 2; }

cleanup() {
    [ "$cleaned" -eq 1 ] && return
    cleaned=1
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT

# Pick a free port: bind, read it, release (a tiny race — fine for a smoke).
PORT="$(python3 - <<'PY'
import socket
s = socket.socket()
s.bind(('127.0.0.1', 0))
print(s.getsockname()[1])
s.close()
PY
)"
[ -n "$PORT" ] || { echo "ABORT: could not pick a free port" >&2; exit 2; }

# ─── Miniature world: a dist dir + a bundle that embeds it exactly ────────
mkdir -p "$TMP/dist"
printf 'hello' > "$TMP/dist/index.html"          # 5 bytes
printf 'ico'   > "$TMP/dist/favicon.ico"         # 3 bytes
python3 - "$TMP/bundle.c" <<'PY'
import sys
open(sys.argv[1], 'w').write('''// Miniature bundle for tests/webapp_served_smoke.sh
#include <stdint.h>
#include "bundle.h"
static const uint8_t _bundle_index_html[] = { 0x68, 0x65, 0x6c, 0x6c, 0x6f };
static const uint8_t _bundle_favicon_ico[] = { 0x69, 0x63, 0x6f };
static const struct BundleAsset _bundle_table[] = {
    { "/",            "text/html",    _bundle_index_html, 5U },
    { "/index.html",  "text/html",    _bundle_index_html, 5U },
    { "/favicon.ico", "image/x-icon", _bundle_favicon_ico, 3U },
    { 0, 0, 0, 0 }
};
''')
PY

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$TMP/dist" >/dev/null 2>&1 &
SERVER_PID=$!
up=0
for i in $(seq 1 20); do
    if curl -sf --max-time 2 "http://127.0.0.1:$PORT/index.html" >/dev/null 2>&1; then up=1; break; fi
    sleep 0.25
done
[ "$up" -eq 1 ] || { echo "ABORT: test HTTP server did not come up" >&2; exit 2; }

verdict() { bash "$GUARD" --url "http://127.0.0.1:$PORT" --bundle "$TMP/bundle.c" >/dev/null 2>&1 && echo PASS || echo FAIL; }
expect() { # $1 = wanted verdict, $2 = what is being tested
    got="$(verdict)"
    if [ "$got" = "$1" ]; then echo "ok:   guard says $got -- $2"; pass=$((pass+1))
    else echo "FAIL: guard says $got, wanted $1 -- $2"; fail=$((fail+1)); fi
}

expect PASS "a live server serving exactly the miniature bundle"

# ─── teeth: every one of these MUST be caught ──────────────────────────────
# 1. Served content drifts: index.html grows by one byte on the server.
printf '!' >> "$TMP/dist/index.html"
expect FAIL "served index.html is one byte longer than embedded"

# 2. An asset vanishes from the server entirely (fetch failure, not drift).
rm -f "$TMP/dist/favicon.ico"
expect FAIL "favicon.ico no longer served at all"

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
