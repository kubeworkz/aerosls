#!/usr/bin/env bash
# tests/entropy_source_smoke.sh — proves the entropy-source guard's teeth bite,
# by planting known-bad and known-good code and asserting the verdict on each.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/entropy_source_check.sh asserts the property §0 of the TLS design doc
# calls the whole project: that no caller uses entropy_get()'s output without
# checking whether there IS any. On failure the buffer is UNTOUCHED, so an
# ignored return does not yield a predictable key -- it yields whatever was on
# the stack, published as key material.
#
# The guard had no smoke twin, and its rule 3 was wrong in both directions for
# its entire life. It matched LINES against a deny-list of tokens
# (= == != < > return if( while( || &&), which means:
#
#   - it failed a deploy on kernel/tls_cert.c:28, a COMMENT about this rule;
#   - and it passed `node->seeded = 1; entropy_get(buf, n);`, because an
#     unrelated `=` anywhere on the line suppressed it.
#
# The false negative is the one that matters, and nothing would ever have
# found it, because a guard with no smoke test is only observed when it fires.
# This file is the missing observation.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh and deploy.sh gates on that same set.
# This script writes files into the tree on purpose. It must never run in the
# deploy gate. Run it with the other build-host smokes.
set -u

GUARD=tests/entropy_source_check.sh
PLANT=kernel/zz_entropy_smoke_plant.c
pass=0; fail=0

# The plant is a .c file inside kernel/. If it survives this script, the next
# `make` compiles it and makefile_sources_check.sh fails on it -- so cleanup
# has to be verified, not attempted. `rm -f` returns 0 when it is denied
# permission on some mounts, and that is not hypothetical: an interrupted run
# of this script left kernel/zz_entropy_smoke_plant.c behind on a filesystem
# where unlink is refused, and the trap reported nothing.
cleanup() {
    rm -f "$PLANT" 2>/dev/null
    if [ -e "$PLANT" ]; then
        echo
        echo "  *** $PLANT COULD NOT BE REMOVED. It is a .c file in kernel/ and"
        echo "      the next build will compile it. Delete it before doing"
        echo "      anything else. ***"
        exit 2
    fi
}
trap cleanup EXIT

verdict() { bash "$GUARD" >/dev/null 2>&1 && echo PASS || echo FAIL; }

expect() { # $1 = wanted verdict, $2 = what was planted
    got="$(verdict)"
    if [ "$got" = "$1" ]; then echo "ok:   guard says $got -- $2"; pass=$((pass+1))
    else echo "FAIL: guard says $got, wanted $1 -- $2"; fail=$((fail+1)); fi
}

rm -f "$PLANT"
expect PASS "the clean tree"

# ─── teeth: every one of these MUST be caught ──────────────────────────────
cat > "$PLANT" <<'EOF'
#include "entropy.h"
void zz_a(void) { unsigned char b[32]; entropy_get(b, sizeof b); }
EOF
expect FAIL "a bare entropy_get(); statement"

cat > "$PLANT" <<'EOF'
#include "entropy.h"
void zz_b(void) { unsigned char b[32]; entropy_get(b, 32);   /* x = 1 hides it */ }
EOF
expect FAIL "a bare call whose trailing COMMENT contains '=' (old rule missed this)"

cat > "$PLANT" <<'EOF'
#include "entropy.h"
struct N { int seeded; };
void zz_c(struct N *n) { unsigned char b[32]; n->seeded = 1; entropy_get(b, 32); }
EOF
expect FAIL "a bare call sharing a line with an assignment (old rule missed this)"

cat > "$PLANT" <<'EOF'
#include "entropy.h"
void zz_d(void) { unsigned char b[32]; (void)entropy_get(b, 32); }
EOF
expect FAIL "an explicit (void) cast -- discarding on purpose is still discarding"

# ─── and every one of these must NOT be ────────────────────────────────────
cat > "$PLANT" <<'EOF'
#include "entropy.h"
/* Prose about entropy_get() and its contract. On failure the buffer is
 * UNTOUCHED, so entropy_get(buf, n) callers must inspect the return. */
// A line comment mentioning entropy_get(b, 32); as well.
void zz_e(void) { int x = 0; (void)x; }
EOF
expect PASS "entropy_get() named only inside comments (this failed a real deploy)"

cat > "$PLANT" <<'EOF'
#include "entropy.h"
int zz_f(void) { unsigned char b[32]; int rc = entropy_get(b, sizeof b); return rc; }
EOF
expect PASS "the return assigned"

cat > "$PLANT" <<'EOF'
#include "entropy.h"
int zz_g(void) { unsigned char b[32]; if (entropy_get(b, 32) != 0) return -1; return 0; }
EOF
expect PASS "the return tested inline"

cat > "$PLANT" <<'EOF'
#include "entropy.h"
int zz_h(void) {
    unsigned char b[32];
    int rc =
        entropy_get(b, 32);
    return rc;
}
EOF
expect PASS "an assignment split across two lines (a line matcher would flag this)"

cleanup
expect PASS "the tree is clean again"

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
