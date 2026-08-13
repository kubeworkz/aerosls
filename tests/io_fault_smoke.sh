#!/usr/bin/env bash
# tests/io_fault_smoke.sh — proves the I/O-error sweep's teeth bite, by
# planting a swallowed return and requiring the sweep to catch it.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/io_fault_host_test.c is a fault-injection sweep in SQLite's shape: fail
# the Nth I/O operation, for every N, and require that the caller was told.
# Its first target is kernel/tls_store.c, which checks every return -- so a
# correct sweep reports zero swallowed failures there.
#
# Zero is also what a BROKEN sweep reports. It is what a sweep reports if the
# injector never injects, if the workload does no I/O, or if the
# "did anything notice?" predicate is wrong. This project has now shipped three
# guards whose teeth had fallen out -- the disk-containment scan counted its
# results inside a pipeline and would have exited 0 on a disk covered in keys --
# so a green sweep is not evidence until it has been shown to go red.
#
# So: take a copy of tls_store.c, delete the `!= 0` on one nvme_write_sync(),
# and require the sweep to fail. That mutation is precisely the defect the
# sweep exists to find, and it is what eight call sites elsewhere in the kernel
# do today.
#
# Exit: 0 pass, 1 fail, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
BUILD="${BUILD:-/tmp/aerosls-io-fault}"
pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

command -v "$CC" >/dev/null 2>&1 || { echo "ABORT: $CC not available." >&2; exit 2; }
for f in tests/io_fault_host_test.c tests/fake_nvme.c kernel/tls_store.c; do
    [ -f "$f" ] || { echo "ABORT: $f missing." >&2; exit 2; }
done

# Artifacts under $BUILD, never in the tree. An interrupted run must not leave
# a .c file behind for `make` to compile -- entropy_source_smoke.sh planted one
# in kernel/ and an interrupted run left it there, on a filesystem whose rm was
# refused.
rm -rf "$BUILD"; mkdir -p "$BUILD"

FLAGS="-Wall -Wextra -std=c11 -D_GNU_SOURCE -I tests -I kernel -I."

echo "=== 1. the sweep against the real kernel/tls_store.c ==="
echo
if ! $CC $FLAGS -o "$BUILD/sweep" tests/io_fault_host_test.c tests/fake_nvme.c \
        kernel/tls_store.c 2>"$BUILD/cc.log"; then
    bad "the sweep did not compile:"; sed 's/^/        /' "$BUILD/cc.log"
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
[ -s "$BUILD/cc.log" ] && { echo "      compiler warnings:"; sed 's/^/        /' "$BUILD/cc.log"; }

OUT="$("$BUILD/sweep" 2>&1)"; rc=$?
echo "$OUT" | sed 's/^/    /'
if [ "$rc" -eq 0 ]; then
    ok "the shipping tls_store.c reports every injected I/O failure"
else
    bad "the shipping tls_store.c SWALLOWS an I/O failure -- this is a real
        defect, not a harness problem. Read the sweep output above."
fi

echo
echo "=== 2. the teeth: a swallowed return must be caught ==="
echo

# The mutation. nvme_write_sync()'s result feeds a ternary in tls_store_save();
# replacing the comparison with something always-true makes the function report
# success no matter what the disk did. sed with @ as the delimiter, because the
# pattern contains == and a / delimiter against a pattern containing / is how a
# mutation silently fails to apply and gets reported SURVIVED.
mkdir -p "$BUILD/mut"
sed 's@(nvme_write_sync(PERSIST_TLS_LBA, ts_frame) == 0)@(nvme_write_sync(PERSIST_TLS_LBA, ts_frame), 1)@' \
    kernel/tls_store.c > "$BUILD/mut/tls_store.c"

if cmp -s kernel/tls_store.c "$BUILD/mut/tls_store.c"; then
    bad "the mutation did not apply -- the pattern no longer matches
        kernel/tls_store.c, so this smoke is testing nothing. Fix the pattern
        before trusting section 1."
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "the mutation applied (the write's status is now ignored)"

if ! $CC $FLAGS -o "$BUILD/sweep_mut" tests/io_fault_host_test.c tests/fake_nvme.c \
        "$BUILD/mut/tls_store.c" 2>"$BUILD/mutcc.log"; then
    bad "the mutated tls_store.c did not compile:"; sed 's/^/        /' "$BUILD/mutcc.log"
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

MOUT="$("$BUILD/sweep_mut" 2>&1)"; mrc=$?
if [ "$mrc" -ne 0 ] && echo "$MOUT" | grep -q "were SWALLOWED"; then
    ok "the sweep CAUGHT it -- teeth confirmed"
    echo "$MOUT" | grep -E "SWALLOWED|NOTHING reported" | sed 's/^/        /'
else
    bad "the sweep did NOT catch a deliberately swallowed write status.
        It cannot be trusted to catch a real one, and section 1's green line
        above means nothing."
    echo "$MOUT" | tail -6 | sed 's/^/        /'
fi

rm -rf "$BUILD/mut"
echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
