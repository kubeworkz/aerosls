#!/usr/bin/env bash
# tests/webapp_bundle_guard_check.sh — byte guard for the compiled-in
# Navigator webapp bundle.
#
# kernel/webapp_bundle.c is a TRACKED file in X86_C_SRC: the kernel serves
# the SPA from it, CI builds it with no ../slsos-sim present at all, and the
# deploy builds from the committed file (deploy.sh no longer pulls slsos-sim
# or runs `make bundle`). So the committed file is CANONICAL: a webapp change
# flows through git (update slsos-sim -> `make bundle` -> commit the
# regenerated kernel/webapp_bundle.c), never through a build host.
#
# This guard proves two things:
#   1. INTERNAL CONSISTENCY (always runs): the header's Files/Total lines
#      agree with the actual arrays and the _bundle_table entries, and every
#      table entry references a real array with the right byte count. Catches
#      a hand-edit, a truncation, or a botched regeneration -- even on a
#      host with no ../slsos-sim.
#   2. SOURCE MATCH (when ../slsos-sim exists, i.e. on the deploy server):
#      the committed file is byte-identical to what tools/bundle_webapp.py
#      generates from ../slsos-sim/dist right now, and the recorded
#      slsos-sim revision in the header matches the sibling checkout's HEAD
#      (when it is a git checkout). Catches the 2026-08-18 failure mode: a
#      webapp advance that was deployed without ever being committed.
#
# GUARD-KIND: host (plain bash + python3; ../slsos-sim only needed for the
# source-match half -- the internal-consistency half always runs).
#
# Exit: 0 pass, 1 fail (drift), 2 abort (missing prerequisites).
set -u
cd "$(dirname "$0")/.."   # repo root

BUNDLE="kernel/webapp_bundle.c"
TOOL="tools/bundle_webapp.py"
DIST="../slsos-sim/dist"

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
[ -f "$BUNDLE" ] || { echo "ABORT: $BUNDLE not found" >&2; exit 2; }
[ -f "$TOOL" ]   || { echo "ABORT: $TOOL not found" >&2; exit 2; }

# ─── 1. Internal consistency (always) ─────────────────────────────────────
python3 - "$BUNDLE" <<'PY'
import re, sys
data = open(sys.argv[1]).read()

m_files = re.search(r'^// Files:\s+(\d+)\s*$', data, re.M)
m_total = re.search(r'^// Total:\s+([\d,]+) bytes', data, re.M)
if not m_files or not m_total:
    print("FAIL  header Files/Total lines missing or malformed")
    sys.exit(1)
decl_files = int(m_files.group(1))
decl_total = int(m_total.group(1).replace(',', ''))

arrays = {}
for m in re.finditer(r'static const uint8_t (_bundle_\w+)\[\] = \{(.*?)\};', data, re.S):
    name, body = m.group(1), m.group(2)
    n = len(re.findall(r'0x[0-9a-fA-F]{2}', body))
    if name in arrays and arrays[name] != n:
        print("FAIL  array %s declared twice with different lengths" % name)
        sys.exit(1)
    arrays[name] = n

tbl = re.search(r'static const struct BundleAsset _bundle_table\[\] = \{(.*?)\};', data, re.S)
if not tbl:
    print("FAIL  _bundle_table not found")
    sys.exit(1)
entries = []
for m in re.finditer(r'\{\s*"([^"]*)",\s*"([^"]*)",\s*(_bundle_\w+),\s*(\d+)U?\s*\}', tbl.group(1)):
    entries.append((m.group(1), m.group(2), m.group(3), int(m.group(4))))
if not entries:
    print("FAIL  no _bundle_table entries parsed")
    sys.exit(1)

for path, mime, ident, length in entries:
    if ident not in arrays:
        print("FAIL  table entry %s references missing array %s" % (path, ident))
        sys.exit(1)
    if arrays[ident] != length:
        print("FAIL  table entry %s: array %s holds %d bytes, table says %d"
              % (path, ident, arrays[ident], length))
        sys.exit(1)

if len(arrays) != decl_files:
    print("FAIL  header says %d files, found %d arrays" % (decl_files, len(arrays)))
    sys.exit(1)
if sum(arrays.values()) != decl_total:
    print("FAIL  header Total %d != sum of arrays %d" % (decl_total, sum(arrays.values())))
    sys.exit(1)

print("OK    internal consistency: %d files, %d bytes, %d table entries"
      % (decl_files, decl_total, len(entries)))
PY
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

# ─── 2. Source match (only where ../slsos-sim exists) ─────────────────────
RECORDED_REV="$(sed -nE 's#^// Source: slsos-sim/dist \(rev ([0-9a-f]+|unknown)\).*#\1#p' \
    "$BUNDLE" | head -1)"

if [ ! -d "$DIST" ]; then
    if [ -d ../slsos-sim/.git ] && [ -n "$RECORDED_REV" ] && [ "$RECORDED_REV" != "unknown" ]; then
        HEAD_REV="$(cd ../slsos-sim && git rev-parse --short=12 HEAD 2>/dev/null)"
        if [ -n "$HEAD_REV" ] && [ "$HEAD_REV" != "$RECORDED_REV" ]; then
            echo "FAIL  slsos-sim is at $HEAD_REV but the committed bundle records $RECORDED_REV"
            echo "      -- a webapp advance that was never committed. Run 'make bundle' and"
            echo "      COMMIT the regenerated kernel/webapp_bundle.c before deploying."
            exit 1
        fi
    fi
    echo "NOTE  $DIST not present -- source-match half skipped; internal consistency"
    echo "      verified above. (CI has no sibling repo; the deploy server does.)"
    echo "webapp_bundle_guard_check: done, 0 violations"
    exit 0
fi

TMP="$(mktemp)"
trap 'rm -f "$TMP"' EXIT
if ! python3 "$TOOL" "$DIST" > "$TMP" 2>/dev/null; then
    echo "ABORT: could not regenerate bundle from $DIST" >&2
    exit 2
fi

if ! cmp -s "$BUNDLE" "$TMP"; then
    echo "FAIL  kernel/webapp_bundle.c differs from what $DIST generates:"
    diff -u "$BUNDLE" "$TMP" 2>/dev/null | head -20 | sed 's/^/      /'
    echo "      The committed bundle is canonical -- regenerate with 'make bundle',"
    echo "      review the diff, and COMMIT the result before deploying."
    exit 1
fi
echo "OK    kernel/webapp_bundle.c matches $DIST byte-for-byte"

if [ -d ../slsos-sim/.git ] && [ -n "$RECORDED_REV" ] && [ "$RECORDED_REV" != "unknown" ]; then
    HEAD_REV="$(cd ../slsos-sim && git rev-parse --short=12 HEAD 2>/dev/null)"
    if [ -n "$HEAD_REV" ] && [ "$HEAD_REV" != "$RECORDED_REV" ]; then
        echo "FAIL  slsos-sim is at $HEAD_REV but the committed bundle records $RECORDED_REV"
        echo "      -- a webapp advance that was never committed. Run 'make bundle' and"
        echo "      COMMIT the regenerated kernel/webapp_bundle.c before deploying."
        exit 1
    fi
    echo "OK    slsos-sim HEAD ($HEAD_REV) matches the recorded revision"
fi

echo "webapp_bundle_guard_check: done, 0 violations"
exit 0
