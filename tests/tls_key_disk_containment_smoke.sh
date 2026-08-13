#!/usr/bin/env bash
# tests/tls_key_disk_containment_smoke.sh — the CA private key appears ONCE on
# the whole disk, and it is in the frame that is supposed to hold it.
#
# ─── The half tests/tls_key_containment_check.sh could not do ──────────────
# That guard proves two things: structurally, that only kernel/tls_store.c
# names PERSIST_TLS_LBA or calls the store; and behaviourally, on a simulated
# disk, that save() puts the key in one frame and leaves none of it in memory.
# Its own header says what it cannot do -- prove that a RUNNING kernel, which
# has since checkpointed, snapshotted and streamed, did not write the key
# somewhere else. §6.1 wrote that gap down as "the structural argument says it
# cannot be there; nobody has looked".
#
# This looks. It reads a real node's disk after it has run.
#
# ─── Why it searches for the SCALAR, not the DER ───────────────────────────
# Searching the image for the stored key's DER encoding would only catch a
# byte-for-byte copy. Any path that re-encoded it -- PKCS#8 instead of SEC1, a
# raw dump of the private scalar, a struct with the key inline -- would slip
# straight past, and those are exactly the shapes a checkpoint or a migration
# stream would produce, because none of them would be copying tls_store.c's
# frame verbatim.
#
# So it parses the SEC1 ECPrivateKey and extracts the 32-byte private scalar,
# which is the secret itself and is identical in every encoding of it. One
# occurrence on a 10 GiB disk means the key is where it is supposed to be and
# nowhere else. Two means something copied it, and the second offset says what.
#
# ─── smoke, not check ──────────────────────────────────────────────────────
# It needs a disk image from a node that has actually run. deploy.sh's gate has
# no such thing, and a guard that skips in the gate protects nothing. Run it
# after a cluster has been up.
#
# Usage:
#   tests/tls_key_disk_containment_smoke.sh --image cluster/node1.img
#   tests/tls_key_disk_containment_smoke.sh --self-test
#
# Exit: 0 pass, 1 fail, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

IMAGE=""; SELFTEST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="${2:-}"; shift 2 ;;
        --self-test) SELFTEST=1; shift ;;
        *) echo "usage: $0 --image FILE | --self-test" >&2; exit 2 ;;
    esac
done

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not available." >&2; exit 2; }

# The LBA comes from persist.h, not from a number typed here. A guard that
# hardcodes the address it is guarding stops guarding the moment the address
# moves, and reports success while reading whatever now lives at the old one.
LBA="$(sed -n 's/^#define PERSIST_TLS_LBA  *\([0-9]*\)ULL.*/\1/p' kernel/persist.h)"
MAGIC="$(sed -n 's/^#define PERSIST_TLS_MAGIC  *\(0x[0-9A-Fa-f]*\)ULL.*/\1/p' kernel/persist.h)"
HDR="$(sed -n 's/^#define TLS_STORE_HDR_BYTES  *\([0-9]*\)u.*/\1/p' kernel/tls_store.h)"
if [ -z "$LBA" ] || [ -z "$MAGIC" ] || [ -z "$HDR" ]; then
    echo "ABORT: could not read PERSIST_TLS_LBA / PERSIST_TLS_MAGIC / TLS_STORE_HDR_BYTES" >&2
    echo "       from the headers. Refusing to guess at the offset -- a wrong one" >&2
    echo "       would search the wrong frame and pass." >&2
    exit 2
fi

SCAN=$(cat <<'PYEOF'
import sys, os

img, lba, magic, hdr = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16), int(sys.argv[4])
SECTOR, FRAME = 512, 4096
off = lba * SECTOR

sz = os.path.getsize(img)
if sz < off + FRAME:
    print("SKIP|image is smaller than the TLS frame offset (%d bytes)" % sz); sys.exit(0)

with open(img, "rb") as f:
    f.seek(off)
    frame = f.read(FRAME)

if int.from_bytes(frame[0:8], "little") != magic:
    print("SKIP|no TLS store on this image -- the node has not generated a CA yet")
    sys.exit(0)

version = int.from_bytes(frame[8:12],  "little")
ca_len  = int.from_bytes(frame[12:16], "little")
key_len = int.from_bytes(frame[16:20], "little")
if ca_len == 0 or key_len == 0 or hdr + ca_len + key_len > FRAME:
    print("FAIL|the frame's own lengths are not self-consistent: ca=%d key=%d" % (ca_len, key_len))
    sys.exit(1)

key_off = off + hdr + ca_len
key = frame[hdr + ca_len : hdr + ca_len + key_len]

# SEC1 ECPrivateKey: SEQUENCE { INTEGER 1, OCTET STRING privateKey, ... }
# The scalar is the secret. Every encoding of this key contains these same 32
# bytes; the DER around them is packaging.
scalar = None
i = key.find(b"\x02\x01\x01\x04\x20")
if i >= 0:
    scalar = key[i + 5 : i + 37]
if scalar is None or len(scalar) != 32:
    print("FAIL|could not locate the 32-byte private scalar in the stored key")
    sys.exit(1)
if scalar == bytes(32):
    print("FAIL|the stored private scalar is all zeroes")
    sys.exit(1)

print("INFO|store v%d at LBA %d: CA %d bytes, key %d bytes" % (version, lba, ca_len, key_len))
print("INFO|private scalar at byte offset %d" % (key_off + i + 5))

# Whole-image scan. Chunked with an overlap so a match spanning a chunk
# boundary is not missed -- which would be a false PASS, the only kind of bug
# this script can have that nobody would notice.
CHUNK = 8 << 20
OVERLAP = 64
hits, scanned = [], 0
with open(img, "rb") as f:
    base = 0
    prev = b""
    while True:
        buf = f.read(CHUNK)
        if not buf:
            break
        hay = prev + buf
        start = 0
        while True:
            j = hay.find(scalar, start)
            if j < 0:
                break
            hits.append(base - len(prev) + j)
            start = j + 1
        scanned += len(buf)
        prev = hay[-OVERLAP:] if len(hay) >= OVERLAP else hay
        base += len(buf)

print("INFO|scanned %d bytes (%.1f GiB)" % (scanned, scanned / (1024.0**3)))
expect = key_off + i + 5
if len(hits) == 1 and hits[0] == expect:
    print("PASS|the private scalar appears exactly once, in the TLS frame")
elif len(hits) == 0:
    print("FAIL|the private scalar was not found AT ALL -- including where it was read from. The scan is broken, not the kernel.")
    sys.exit(1)
else:
    print("FAIL|the private scalar appears %d times on this disk" % len(hits))
    for h in hits:
        where = "the TLS frame" if h == expect else "LBA %d (byte %d) -- NOT the TLS frame" % (h // SECTOR, h)
        print("FAIL|  offset %d: %s" % (h, where))
    sys.exit(1)
PYEOF
)

run_scan() {
    python3 -c "$SCAN" "$1" "$LBA" "$MAGIC" "$HDR"
}

pass=0; fail=0
# NOT a pipeline. `run_scan | report` reads correctly and counts into a
# SUBSHELL, so pass/fail come back zero and the script exits 0 no matter what
# the scan found -- a containment guard that always passes. The self-test
# below is what caught it: it printed two ok: lines and tallied one. Every
# line of this script's real output goes through here, so that subshell would
# have swallowed a genuine leak silently.
report() {
    local kind msg
    while IFS='|' read -r kind msg; do
        case "$kind" in
            INFO) echo "      $msg" ;;
            PASS) echo "ok:   $msg"; pass=$((pass+1)) ;;
            FAIL) echo "FAIL: $msg"; fail=$((fail+1)) ;;
            SKIP) echo "SKIP  $msg" ;;
        esac
    done <<< "$1"
}

# ─── self-test: prove the scan bites before trusting it on a real image ────
# A containment scan that cannot fail is indistinguishable from one that passes,
# and this project has shipped two guards whose teeth had fallen out. So: build
# a synthetic image with a planted store, confirm one hit; plant a SECOND copy
# of the scalar elsewhere, confirm it is found and named.
if [ "$SELFTEST" -eq 1 ]; then
    echo "=== self-test: the scan's teeth ==="
    echo
    T="${TMPDIR:-/tmp}/tls-disk-containment-selftest"
    rm -rf "$T"; mkdir -p "$T"
    python3 - "$T" "$LBA" "$MAGIC" "$HDR" <<'PYEOF'
import sys, os
T, lba, magic, hdr = sys.argv[1], int(sys.argv[2]), int(sys.argv[3], 16), int(sys.argv[4])
SECTOR, FRAME = 512, 4096
off = lba * SECTOR
size = off + FRAME + (4 << 20)          # room after the frame to hide a copy in

scalar = bytes((i * 7 + 3) & 0xFF for i in range(32))
key = b"\x30\x77\x02\x01\x01\x04\x20" + scalar + b"\xa0\x0a\x06\x08" + b"\x2a" * 8
ca  = bytes((i * 3 + 1) & 0xFF for i in range(451))

frame = bytearray(FRAME)
frame[0:8]   = magic.to_bytes(8, "little")
frame[8:12]  = (1).to_bytes(4, "little")
frame[12:16] = len(ca).to_bytes(4, "little")
frame[16:20] = len(key).to_bytes(4, "little")
frame[24:32] = (1786577553).to_bytes(8, "little")
frame[hdr:hdr+len(ca)] = ca
frame[hdr+len(ca):hdr+len(ca)+len(key)] = key

for name, plant in (("clean.img", False), ("leaked.img", True)):
    p = os.path.join(T, name)
    with open(p, "wb") as f:
        f.truncate(size)
        f.seek(off); f.write(frame)
        if plant:
            # A checkpoint that grabbed the key. Re-encoded, not a frame copy --
            # the shape the DER search would have missed.
            f.seek(off + FRAME + (1 << 20))
            f.write(b"CKPTENTRY" + scalar + b"\x00" * 16)
print("built clean.img and leaked.img")
PYEOF

    echo "--- a disk with the key only where it belongs ---"
    report "$(run_scan "$T/clean.img")"
    echo
    echo "--- the same disk with one re-encoded copy planted 1 MiB later ---"
    out="$(run_scan "$T/leaked.img")"
    echo "$out" | sed 's/^INFO|/      /; s/^FAIL|/    would-fail: /; s/^PASS|/    would-pass: /'
    if echo "$out" | grep -q '^FAIL|the private scalar appears 2 times'; then
        echo "ok:   the planted copy IS found -- the scan has teeth"
        pass=$((pass+1))
    else
        echo "FAIL: the planted copy was NOT found. This scan cannot be trusted on a"
        echo "      real image: it would report containment it never checked."
        fail=$((fail+1))
    fi
    rm -rf "$T"
    echo
    echo "---- passed=$pass failed=$fail"
    [ "$fail" -eq 0 ] || exit 1
    exit 0
fi

[ -n "$IMAGE" ] || { echo "usage: $0 --image FILE | --self-test" >&2; exit 2; }
[ -f "$IMAGE" ] || { echo "ABORT: no such image: $IMAGE" >&2; exit 2; }

echo "=== the CA private key on $IMAGE ==="
echo
out="$(run_scan "$IMAGE")"; scan_rc=$?
report "$out"
# The scan's own exit code as well as its FAIL lines. Either alone can be
# wrong: a python traceback exits non-zero with no FAIL line, and a future
# edit could print FAIL without exiting. Requiring both to agree means a
# disagreement is itself reported rather than silently resolved in the
# direction of passing.
if [ "$scan_rc" -ne 0 ] && [ "$fail" -eq 0 ]; then
    echo "FAIL: the scan exited $scan_rc without reporting a failure -- it died"
    echo "      partway. Treat this as UNKNOWN, not as containment."
    fail=$((fail+1))
fi
echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
