#!/usr/bin/env bash
# tests/rowstore_io_smoke.sh — the row store reports a write it could not make.
#
# ─── What this guards ──────────────────────────────────────────────────────
# rowstore_flush_page() returned void and discarded nvme_write_sync()'s status,
# so rowstore_row_insert/update/delete returned 0 whatever the disk did. The
# row was in RAM, the caller was told it was stored, it was gone on the next
# boot, and nothing logged anything. tests/io_fault_smoke.sh's seam found it in
# the first module it was aimed at that does not check its returns.
#
# It now returns ROWSTORE_RC_NOT_DURABLE, which is deliberately NOT an error:
# the row exists and is readable. A caller that treated it as "insert failed"
# and retried would get two rows. This asserts both halves -- that the failure
# is reported, and that the row is still there.
#
# ─── And the teeth ─────────────────────────────────────────────────────────
# Section 2 puts the void return back, in a copy, and requires the harness to
# fail. Four guards this session were shipped toothless and caught, one of them
# because its assertion was subtly the wrong assertion rather than absent. A
# green line is not evidence until it has been shown to go red.
#
# Exit: 0 pass, 1 fail, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
BUILD="${BUILD:-/tmp/aerosls-rowstore-io}"
pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

command -v "$CC" >/dev/null 2>&1 || { echo "ABORT: $CC not available." >&2; exit 2; }
for f in tests/rowstore_io_host_test.c tests/rowstore_io_stubs.c tests/fake_nvme.c \
         kernel/rowstore.c; do
    [ -f "$f" ] || { echo "ABORT: $f missing." >&2; exit 2; }
done

# Artifacts under $BUILD, never in the tree: entropy_source_smoke.sh planted a
# .c file in kernel/ and an interrupted run left it there, on a filesystem
# whose rm was refused.
rm -rf "$BUILD"; mkdir -p "$BUILD"
FLAGS="-Wall -Wextra -std=c11 -D_GNU_SOURCE -I tests -I kernel -I. -Inet -Iarch/x86"

echo "=== 1. the shipping kernel/rowstore.c ==="
echo
if ! $CC $FLAGS -o "$BUILD/rs" tests/rowstore_io_host_test.c tests/rowstore_io_stubs.c \
        tests/fake_nvme.c kernel/rowstore.c 2>"$BUILD/cc.log"; then
    bad "the harness did not compile:"; sed 's/^/        /' "$BUILD/cc.log"
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
[ -s "$BUILD/cc.log" ] && { echo "      compiler warnings:"; sed 's/^/        /' "$BUILD/cc.log"; }

OUT="$("$BUILD/rs" 2>&1)"; rc=$?
echo "$OUT" | sed 's/^/    /'
if [ "$rc" -eq 0 ]; then
    ok "a failed page write is reported, and the row is still live"
else
    bad "the row store does not report a failed page write. Read the output above."
fi

echo
echo "=== 2. the teeth: put the swallowed status back ==="
echo
mkdir -p "$BUILD/mut"
# @ as the delimiter: the pattern contains / in the LBA arithmetic, and a
# delimiter collision is how a mutation silently fails to apply and gets
# reported SURVIVED. This project has lost two mutation runs to exactly that.
sed 's@if (nvme_write_sync(ROWSTORE_LBA_BASE + (uint64_t)page_id \* 8,@if (0 \&\& nvme_write_sync(ROWSTORE_LBA_BASE + (uint64_t)page_id * 8,@' \
    kernel/rowstore.c > "$BUILD/mut/rowstore.c"

if cmp -s kernel/rowstore.c "$BUILD/mut/rowstore.c"; then
    bad "the mutation did not apply -- the pattern no longer matches
        kernel/rowstore.c, so this smoke is testing nothing. Fix the pattern
        before trusting section 1."
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "the mutation applied (the write's status is ignored again)"

if ! $CC $FLAGS -o "$BUILD/rs_mut" tests/rowstore_io_host_test.c tests/rowstore_io_stubs.c \
        tests/fake_nvme.c "$BUILD/mut/rowstore.c" 2>"$BUILD/mutcc.log"; then
    bad "the mutated rowstore.c did not compile:"; sed 's/^/        /' "$BUILD/mutcc.log"
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

MOUT="$("$BUILD/rs_mut" 2>&1)"; mrc=$?
if [ "$mrc" -ne 0 ] && echo "$MOUT" | grep -q "returned SUCCESS while every write"; then
    ok "the harness CAUGHT it -- teeth confirmed"
    echo "$MOUT" | grep -E "returned SUCCESS" | sed 's/^/        /'
else
    bad "the harness did NOT catch a deliberately swallowed write status.
        Section 1's green line above means nothing."
    echo "$MOUT" | tail -6 | sed 's/^/        /'
fi

rm -rf "$BUILD/mut"
echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
