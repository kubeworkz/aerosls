#!/usr/bin/env bash
# tests/simi_blob_guard_check.sh — CI-style byte guard for the generated
# simi_recycle blob header.
#
# simi_recycle.c embeds SIMI_RECYCLE_TMO_BLOB (user/examples/
# simi_recycle_tmo_blob.h), which the Makefile regenerates from
# tools/simi/tests/add.tmo on every build — but a hand-edit of the header
# is NEWER than the .tmo, so make's timestamps would never regenerate it.
# This guard re-extracts the embedded bytes and compares them byte-for-byte
# against the source .tmo (reassembling the .tmo from add.simi via the SIMI
# host toolchain if a fresh checkout hasn't built it yet), so a hand-edit or
# a broken regeneration is caught even when make thinks everything is fresh.
#
# GUARD-KIND: host (plain bash + python3 + cc; needs the tools/simi
# toolchain only on a fresh checkout where add.tmo isn't built yet).
#
# Exit: 0 pass, 1 fail (drift), 2 abort (missing prerequisites).
set -u
cd "$(dirname "$0")/.."   # repo root

HDR="user/examples/simi_recycle_tmo_blob.h"
SRC="tools/simi/tests/add.simi"
TMO="tools/simi/tests/add.tmo"

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }
[ -f "$HDR" ] || { echo "ABORT: $HDR not found" >&2; exit 2; }
[ -f "$SRC" ] || { echo "ABORT: $SRC not found" >&2; exit 2; }

# The .tmo is a build output (tools/simi keeps build outputs untracked) —
# reassemble it from source if a fresh checkout hasn't built it yet.
if [ ! -f "$TMO" ]; then
    if [ ! -x tools/simi/simi-asm ]; then
        (cd tools/simi && make simi-asm >/dev/null 2>&1) || {
            echo "ABORT: could not build tools/simi/simi-asm" >&2; exit 2; }
    fi
    (cd tools/simi && ./simi-asm tests/add.simi tests/add.tmo >/dev/null 2>&1) || {
        echo "ABORT: could not reassemble $TMO from $SRC" >&2; exit 2; }
fi

python3 - "$HDR" "$TMO" <<'PY'
import re, sys
hdr, tmo = sys.argv[1], sys.argv[2]
hexs = re.findall(r'0x([0-9a-fA-F]{2})', open(hdr).read())
blob = bytes(int(h, 16) for h in hexs)
ref  = open(tmo, 'rb').read()
if blob != ref:
    print("FAIL  simi_recycle_tmo_blob.h drifted from %s:" % tmo)
    print("      embedded %d bytes, source %d bytes" % (len(blob), len(ref)))
    sys.exit(1)
print("OK    simi_recycle_tmo_blob.h matches %s byte-for-byte (%d bytes)"
      % (tmo, len(blob)))
PY
rc=$?
[ "$rc" -eq 0 ] || exit "$rc"

echo "simi_blob_guard_check: done, 0 violations"
exit 0
