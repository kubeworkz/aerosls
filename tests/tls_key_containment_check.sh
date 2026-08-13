#!/usr/bin/env bash
# tests/tls_key_containment_check.sh — §6.1: the CA private key goes to ONE
# place, and nothing else may learn where that place is.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/tls_store.c writes the node's CA private key to NVMe in plaintext, at
# a fixed LBA, on an unencrypted disk. That is a deliberate trade and
# kernel/tls_store.h states it: reboot-survivable trust is not available
# without a secret that survives reboots, and the alternative was re-importing
# a new anchor into two browser trust stores after every boot.
#
# The trade is only defensible while the key goes to exactly one place. A key
# that ALSO reaches a checkpoint, a snapshot or a migration stream has three
# lifetimes, three sets of copies and three audiences -- and every one of those
# paths was written by someone who had no idea a private key would ever pass
# through it. Nothing about that failure is visible: the node works, the
# browsers are happy, and the key is in a stream file on another host.
#
# So the containment is asserted in two ways, because neither is enough alone:
#
#   1. STRUCTURAL. Who is even allowed to name the region or call the store.
#      A grep, and a weak-looking one -- but it is the check that fires when
#      someone adds the TLS frame to a checkpoint walk, because they would
#      have to name PERSIST_TLS_LBA or call tls_store_load() to do it.
#
#   2. BEHAVIOURAL. A host harness with a simulated disk plants a key made of
#      bytes nothing else would produce, and goes looking for it: on the disk
#      (it must be in exactly one frame) and in the module's staging buffer
#      (it must be in none of it, on every path including the early returns).
#      tls_store.h claims the staging frame is zeroized on every path. This is
#      what stops that from being a claim.
#
# ─── What this does NOT cover ──────────────────────────────────────────────
# It cannot prove a RUNNING kernel's checkpoint is clean -- that needs a live
# node and its real disk image, and no such check exists yet. Check 1 covers
# the structural side of it: a checkpoint cannot include the key without
# naming the LBA or calling the store, and both are watched here. That is a
# real argument and it is not the same as having looked. Do not read a green
# line as "the key has been confirmed absent from a written checkpoint".
#
# Runs under tests/run_checks.sh (the tests/*_check.sh glob) and therefore in
# deploy.sh's guard gate.
#
# Exit: 0 pass, 1 fail, 2 prerequisite missing.
set -u
cd "$(dirname "$0")/.."

CC="${CC:-gcc}"
pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

command -v "$CC" >/dev/null 2>&1 || {
    echo "ABORT: $CC not available; this guard cannot run." >&2
    echo "       Passing without compiling would assert containment it never" >&2
    echo "       checked, which is worse than not running." >&2
    exit 2
}
[ -f kernel/tls_store.c ] || { echo "ABORT: kernel/tls_store.c missing." >&2; exit 2; }

echo "=== 1. who may name the region, and who may call the store ==="
echo

# The LBA and its magic. persist.h DEFINES them; tls_store.c is the only file
# allowed to USE them. A second user is a second writer to a region whose whole
# safety argument is that it has one.
for sym in PERSIST_TLS_LBA PERSIST_TLS_MAGIC; do
    # -l, and --cached so this inspects what is committed rather than whatever
    # is lying in the worktree.
    # Exclusions are NAMED, not globbed. 'tests/' as a pattern would also
    # excuse a future test that copied the key somewhere; one filename cannot.
    users="$(git grep -l --cached "$sym" -- '*.c' '*.h' \
             | grep -vE '^(kernel/persist\.h|tests/tls_key_containment\.c)$' | sort)"
    if [ "$users" = "kernel/tls_store.c" ]; then
        ok "$sym is used by kernel/tls_store.c and nothing else"
    elif [ -z "$users" ]; then
        bad "$sym is defined but used by NOTHING -- the store is not wired up"
    else
        bad "$sym is used outside kernel/tls_store.c:"
        echo "$users" | sed 's/^/        /'
        echo "      A new user of this region is a new place the CA key can reach."
    fi
done

# The store's API. tls_server.c is the only PRODUCTION caller; tls_store.h
# declares it and tls_store.c defines it. Anything else -- checkpoint_mgr,
# stream, persist -- is the failure this guard exists for. Two test harnesses
# are named, the same way tls_key_containment.c already is: tls_key_containment.c
# plants a marker key and goes looking for it, and io_fault_host_test.c sweeps
# save/load/wipe against a fake NVMe that can be made to fail. Both are test-only
# callers of the same API the production path uses, so they exercise the real
# store and are named here so they cannot silently multiply.
callers="$(git grep -l --cached -E 'tls_store_(load|save|wipe)\(' -- '*.c' \
           | grep -vE '^(kernel/tls_store\.c|tests/tls_key_containment\.c|tests/io_fault_host_test\.c)$' | sort)"
if [ "$callers" = "kernel/tls_server.c" ]; then
    ok "tls_store_load/save/wipe are called only from kernel/tls_server.c"
elif [ -z "$callers" ]; then
    bad "nothing calls the store -- the CA cannot be surviving a reboot"
else
    bad "the store is called from outside kernel/tls_server.c:"
    echo "$callers" | sed 's/^/        /'
fi

# tls_store.c must not itself reach into the persistence machinery. If it ever
# calls persist_* or checkpoint_*, the region stops being independent of the
# walk that this guard's whole argument rests on.
if git grep -n --cached -E '\b(persist|checkpoint|stream)_[a-z_]+\(' -- kernel/tls_store.c >/dev/null 2>&1; then
    bad "kernel/tls_store.c calls into persist/checkpoint/stream:"
    git grep -n --cached -E '\b(persist|checkpoint|stream)_[a-z_]+\(' -- kernel/tls_store.c | sed 's/^/        /'
else
    ok "kernel/tls_store.c calls no persist/checkpoint/stream function"
fi

# And the CA key buffer in tls_server.c must not be handed to anything that
# writes. This is the narrow, specific version of "do not let it escape".
if git grep -n --cached 'ca_key_der' -- kernel/tls_server.c \
     | grep -E '\b(persist|checkpoint|stream|nvme)_[a-z_]+\(' >/dev/null 2>&1; then
    bad "tls_server.c's ca_key_der is passed to a persistence call:"
    git grep -n --cached 'ca_key_der' -- kernel/tls_server.c \
        | grep -E '\b(persist|checkpoint|stream|nvme)_[a-z_]+\(' | sed 's/^/        /'
else
    ok "tls_server.c's CA key buffer reaches no persistence call directly"
fi

echo
echo "=== 2. where the key actually goes, on a simulated disk ==="

BUILD="${BUILD:-/tmp/aerosls-tls-containment}"
mkdir -p "$BUILD"
# TLS_STORE_TEST_HOOKS exposes the staging frame. It is compiled ONLY here --
# tests/script_conventions_check.sh's sibling argument: a hook the shipping
# kernel can reach is not a hook, it is an API.
if ! $CC -Wall -Wextra -std=c11 -DTLS_STORE_TEST_HOOKS \
        -I kernel -I . -o "$BUILD/containment" \
        tests/tls_key_containment.c kernel/tls_store.c 2>"$BUILD/cc.log"; then
    bad "the containment harness did not compile:"
    sed 's/^/        /' "$BUILD/cc.log"
    echo
    echo "---- passed=$pass failed=$fail"
    exit 1
fi
[ -s "$BUILD/cc.log" ] && { echo "      compiler warnings:"; sed 's/^/        /' "$BUILD/cc.log"; }

HOUT="$("$BUILD/containment" 2>&1)"; hrc=$?
echo "$HOUT" | sed 's/^/    /'
pass=$(( pass + $(echo "$HOUT" | grep -c '^ok:') ))
hf=$(echo "$HOUT" | grep -c '^FAIL:')
fail=$(( fail + hf ))
if [ "$hrc" -ne 0 ] && [ "$hf" -eq 0 ]; then
    bad "the containment harness exited $hrc without reporting a failure"
fi

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
