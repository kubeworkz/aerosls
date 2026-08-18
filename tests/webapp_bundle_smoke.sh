#!/usr/bin/env bash
# tests/webapp_bundle_smoke.sh — proves the webapp bundle guard's teeth bite,
# by corrupting the committed kernel/webapp_bundle.c and asserting FAIL, then
# restoring it byte-for-byte and asserting PASS.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/webapp_bundle_guard_check.sh is a byte guard over a TRACKED 5.9 MB
# generated file. A guard that passes while inspecting nothing is rot, and
# this guard's two halves have opposite blind spots:
#
#   - the internal-consistency half runs everywhere and catches a hand-edit
#     or truncation -- but only if the file actually parses;
#   - the source-match half (byte-compare against ../slsos-sim/dist + the
#     recorded slsos-sim revision) only runs where the sibling repo exists,
#     i.e. the deploy server, and CI has no sibling.
#
# This smoke proves the half CI CAN run (internal consistency) actually
# fires on a planted corruption. It is source-only -- plain bash + python3,
# no build -- so run_source_smokes.sh covers it on every push, independent
# of the kernel build.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh and deploy.sh gates on that same set.
# This script writes into the tree on purpose. It must never run in the
# deploy gate. Run it with the other source smokes.
set -u

GUARD=tests/webapp_bundle_guard_check.sh
BUNDLE=kernel/webapp_bundle.c
BACKUP="$(mktemp)"
pass=0; fail=0
cleaned=0

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
[ -f "$BUNDLE" ] || { echo "ABORT: $BUNDLE not found" >&2; exit 2; }

# The bundle is a tracked file inside kernel/. If it survives this script
# corrupted, the next build embeds a broken webapp and the diff is invisible
# to make (mtime only). Restore has to be verified, not attempted -- the
# entropy_source_smoke precedent: an interrupted run left its plant behind
# on a filesystem where unlink was refused, and the trap reported nothing.
# The explicit call below (to restore before the final PASS check) and the
# EXIT trap share one body; `cleaned` makes the second call a no-op instead
# of cp-ing from a backup that was already removed.
cp "$BUNDLE" "$BACKUP"
cleanup() {
    [ "$cleaned" -eq 1 ] && return
    cleaned=1
    cp "$BACKUP" "$BUNDLE" 2>/dev/null
    if ! cmp -s "$BACKUP" "$BUNDLE"; then
        echo
        echo "  *** $BUNDLE IS NOT BYTE-IDENTICAL TO ITS ORIGINAL. The deployed"
        echo "      webapp would silently change. Restore it from git before"
        echo "      doing anything else. ***"
        exit 2
    fi
    rm -f "$BACKUP"
}
trap cleanup EXIT

verdict() { bash "$GUARD" >/dev/null 2>&1 && echo PASS || echo FAIL; }

expect() { # $1 = wanted verdict, $2 = what was planted
    got="$(verdict)"
    if [ "$got" = "$1" ]; then echo "ok:   guard says $got -- $2"; pass=$((pass+1))
    else echo "FAIL: guard says $got, wanted $1 -- $2"; fail=$((fail+1)); fi
}

expect PASS "the pristine committed bundle"

# ─── teeth: every one of these MUST be caught ──────────────────────────────
# 1. Header Total no longer matches the sum of the arrays.
python3 - "$BACKUP" "$BUNDLE" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src).read()
t = int(re.search(r'^// Total:  ([\d,]+)', data, re.M).group(1).replace(',', ''))
data = re.sub(r'^// Total:  [\d,]+', '// Total:  %d' % (t + 1), data, count=1, flags=re.M)
open(dst, 'w').write(data)
PY
expect FAIL "a Total header that is off by one byte"

# 2. A table entry length that disagrees with its array.
python3 - "$BACKUP" "$BUNDLE" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src).read()
data = re.sub(r'(_bundle_\w+,\s*)(\d+)U?', lambda m: m.group(1) + str(int(m.group(2)) + 1) + 'U', data, count=1)
open(dst, 'w').write(data)
PY
expect FAIL "a table entry whose length disagrees with its array"

# 3. The first array truncated by one byte. The generator emits 16 bytes per
#    line, `0xNN, ` separators, and the final byte bare before `};` -- drop
#    that final token so the array is one byte shorter with everything else
#    intact (make's mtime rule cannot see this; only a byte guard can).
python3 - "$BACKUP" "$BUNDLE" <<'PY'
import re, sys
src, dst = sys.argv[1], sys.argv[2]
data = open(src).read()
m = re.search(r'static const uint8_t (_bundle_\w+)\[\] = \{(.*?)\n\};', data, re.S)
assert m, "could not locate the first array"
body = m.group(2)
tokens = list(re.finditer(r'0x[0-9a-fA-F]{2}', body))
assert tokens, "array body has no bytes"
last = tokens[-1]
new_body = body[:last.start()] + body[last.end():]
data = data[:m.start(2)] + new_body + data[m.end(2):]
open(dst, 'w').write(data)
PY
rc=$?
if [ "$rc" -ne 0 ]; then
    # Plant failed to apply -- the tooth is unproven, and the file may be
    # half-written. Restore and treat the smoke itself as broken.
    cleanup
    echo "FAIL: truncation plant could not be applied (python rc=$rc) -- tooth unproven"
    fail=$((fail + 1))
else
    expect FAIL "the first array truncated by one byte (make's mtime rule would miss this)"
fi

# ─── and the tree must be clean again ──────────────────────────────────────
cleanup
expect PASS "the bundle restored byte-for-byte"

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
