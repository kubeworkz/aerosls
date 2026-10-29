#!/usr/bin/env bash
# env_replica_check.sh — P3 destination (a): the replica path's wiring, static.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# Increment 1 (kernel/env_replica.c) is a receiver whose whole value is
# CONNECTED: a BEGIN that never reaches it, a tick that never finalizes it,
# a ceiling the two receivers disagree about, or a dispatcher left without
# its weak defaults each break the feature in a way no single file review
# shows — and each already has a precedent in this tree's failure history:
#
#   * a routing opcode that exists in the header but no branch dispatches
#     would stage NOTHING while every ACK said READY (the silent half of
#     the failure: the sender sees a perfectly healthy transfer);
#   * dspp.c's weak defaults are the boundary every OTHER host test that
#     links the dispatcher without env_replica.c depends on — deleting
#     them turns six existing suites into link errors, which is at least
#     loud, but the assertion keeps the reason documented next to the code;
#   * ENV_MIGRATE_IMAGE_MAX is the one number BOTH wire receivers gate a
#     BEGIN on (env_migrate.c sends under it, env_replica.c refuses past
#     it). Two definitions would let the receivers drift into each other's
#     refusal — the exact class of bug the shared arithmetic exists to
#     prevent, and one that only ever fires on a live tail transfer;
#   * the band-adjacency static assert is what stops a payload band that
#     grows from being written into by the replica band that follows it.
#
# This guard is SOURCE-ONLY: it reads the checked-in wiring and the host
# test's teeth. No GUARD-KIND marker, on purpose — there is nothing here a
# Linux runner cannot read, so a skip must not be available to it.
#
# Exit: 0 pass, 1 a wiring claim above is violated, 2 a file it reads is missing.
set -u
cd "$(dirname "$0")/.." || exit 1

failed=0
ok()    { echo "ok:   $1"; }
fail()  { echo "FAIL: $1"; failed=1; }
need() {
    if [ ! -f "$1" ]; then
        echo "ABORT: $1 is missing — this guard reads it directly." >&2
        exit 2
    fi
}

need Makefile
need kernel/env_replica.c
need kernel/env_replica.h
need net/dspp.c
need net/dspp.h
need kernel/microkernel.c
need kernel/persist.h
need kernel/env_migrate.h
need tests/env_replica_host_test.c

# 1. The receiver is built into the kernel.
if grep -q 'kernel/env_replica\.c' Makefile; then
    ok "the Makefile builds kernel/env_replica.c into the kernel"
else
    fail "Makefile does not list kernel/env_replica.c — the receiver would not exist at runtime"
fi

# 2. The replica BEGIN has its own opcode, and it is 25.
if grep -qE 'DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ[[:space:]]*=[[:space:]]*25' net/dspp.h; then
    ok "dspp.h assigns the replica BEGIN its own opcode (25)"
else
    fail "dspp.h lacks DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ = 25 — a replica BEGIN cannot be told from a migration BEGIN"
fi

# 3. The dispatch routes that opcode to the replica receiver (a header-only
#    opcode would ACK READY from the MIGRATE receiver's table instead).
if grep -q 'h->opcode == DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ' net/dspp.c &&
   grep -q 'env_replica_recv_begin(' net/dspp.c; then
    ok "dspp.c routes the replica BEGIN to env_replica_recv_begin()"
else
    fail "dspp.c does not route DSPP_MIGRATE_ENV_REPLICA_BEGIN_REQ — the staged slot would belong to the wrong receiver"
fi

# 4. The shared PAGE family follows the receiver that HOLDS the transfer.
if grep -q 'env_replica_holds(h->transfer_id)' net/dspp.c; then
    ok "the PAGE branch routes by env_replica_holds()"
else
    fail "dspp.c's PAGE branch no longer routes by env_replica_holds() — replica pages would land in the migration table"
fi

# 5. The weak defaults stay: they are what lets every dispatcher-only host
#    test link without the replica subsystem (env_migrate, stream, ctx,
#    consensus suites alike).
missing_weak=""
for sym in env_replica_recv_begin env_replica_recv_page env_replica_holds; do
    grep -q "__attribute__((weak)) int ${sym}" net/dspp.c ||
        missing_weak="$missing_weak $sym"
done
if [ -z "$missing_weak" ]; then
    ok "dspp.c keeps its three weak replica defaults (the dispatcher-only link boundary)"
else
    fail "dspp.c lost weak default(s):$missing_weak — host tests linking the dispatcher without env_replica.c stop building"
fi

# 6. The finalize pass is wired into the service poll — a receiver nothing
#    ticks stages frames forever.
if grep -q 'env_replica_tick();' kernel/microkernel.c; then
    ok "microkernel.c calls env_replica_tick() from the service poll"
else
    fail "microkernel.c does not call env_replica_tick() — completed stagings never finalize"
fi

# 7. The directory LBAs, and the adjacency assert that keeps the band from
#    colliding with the payload band it follows.
if grep -qE 'PERSIST_ENV_REPLICA_HDR_LBA[[:space:]]+7760ULL' kernel/persist.h &&
   grep -qE 'PERSIST_ENV_REPLICA_ENT_LBA[[:space:]]+7768ULL' kernel/persist.h; then
    ok "persist.h maps the replica directory at 7760/7768 (under STREAM_DIR 8192)"
else
    fail "persist.h lacks the replica directory at 7760/7768"
fi
if grep -q '_Static_assert(ENV_REPLICA_DATA_LBA_BASE ==' kernel/env_replica.c; then
    ok "env_replica.c asserts the band sits exactly where the payload band ends"
else
    fail "env_replica.c lost the band-adjacency static assert — a grown payload band would be written into"
fi

# 8. ONE image-tail ceiling, in the shared header: both receivers gate a
#    BEGIN on it, so two definitions is two opinions about the same wire.
n_defs=$(grep -rE 'define[[:space:]]+ENV_MIGRATE_IMAGE_MAX' kernel/ | wc -l)
if [ "$n_defs" -eq 1 ] && grep -q 'define ENV_MIGRATE_IMAGE_MAX' kernel/env_migrate.h; then
    ok "ENV_MIGRATE_IMAGE_MAX has exactly one definition, in env_migrate.h (shared by both receivers)"
else
    fail "ENV_MIGRATE_IMAGE_MAX has $n_defs definitions — the sender's ceiling and the replica's gate could drift apart"
fi

# 9. The refusal-by-name contract is PINNED BY NAME in the host test: a
#    code that exists only in the enum is a code nobody can regress
#    quietly. The two codes the test cannot provoke (IO needs a
#    write-failing device fake, INCOMPLETE is the defensive re-check) are
#    the only ones allowed to be absent.
required_codes="NO_RECORD RECORD_LEN PAGE_COUNT TOO_MANY SLOT_EXISTS TABLE_FULL
                NO_DISK_SLOT SUPERSEDED UNKNOWN RANGE ALLOC STALE"
unpinned=""
for code in $required_codes; do
    grep -q "ENV_REPLICA_REFUSE_${code}" tests/env_replica_host_test.c ||
        unpinned="$unpinned $code"
done
if [ -z "$unpinned" ]; then
    ok "the host test pins every pinnable refusal code by name"
else
    fail "refusal code(s) no longer asserted by name in the host test:$unpinned"
fi

if [ "$failed" -ne 0 ]; then
    echo "env_replica_check: FAILED"
    exit 1
fi
echo "env_replica_check: all wiring claims hold"
exit 0
