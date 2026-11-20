#!/usr/bin/env bash
# tests/failover_refused_classification_check.sh — permanent regression test
# for failover_adoption_live_check.sh's restore-refused classification.
#
# The live guard used to treat a promoted replica whose replay was refused by
# env_migrate's placement gate as "the environment was never named", then wait
# out WAIT_ADOPT (240 s) before failing. The node1 log that exposed this has
# three decisive lines after the adoption:
#
#   [ENV_REPLICA] PROMOTED replica (partition 6, index 1): ...
#   [ENV_MIGRATE] restore (partition 6, index 1): region kind 1: ...
#   [FAILOVER] environment index 1 (partition 6, 3 region(s)) was NOT
#   restored -- its replica staged on this survivor but the replay refused it
#   (fresh environment landed off the staged bases); recreate it
#
# This test does not stand up three QEMU nodes. It extracts the production
# `restore_refused_line()` helper from the guard and feeds it a small fixture
# derived from those lines, so the matcher cannot regress to the old path.
# It also pins the guard's call site and its position before the "never named"
# fallback, because removing the call would recreate the same live defect even
# if the helper stayed correct.
#
# Exit: 0 pass, 1 a claim failed, 2 the guard or fixture tooling is missing.
set -u
cd "$(dirname "$0")/.." || exit 1

GUARD=tests/failover_adoption_live_check.sh
[ -f "$GUARD" ] || { echo "ABORT: $GUARD is missing."; exit 2; }

STATE="$(mktemp -d)" || { echo "ABORT: mktemp -d failed."; exit 2; }
trap 'rm -rf "$STATE"' EXIT
FIXTURE="$STATE/node1-refused.log"
BAD="$STATE/promotion-after-refusal.log"

# Small fixture derived from the preserved node1 log. Lines 1-2 are stale
# evidence from before the adoption and must not satisfy the classifier; 3 is
# the adopted row; 5-6 are the live promotion and refusal; 7-8 belong to a
# different partition and must not match partition 6.
cat > "$FIXTURE" <<'EOF'
[ENV_REPLICA] PROMOTED replica (partition 6, index 1): 1970 page(s), capture seq 0, provenance node 2 -- staged for the migration restore path.
[FAILOVER] environment index 1 (partition 6, 3 region(s)) was NOT restored -- its replica staged on this survivor but the replay refused it (fresh environment landed off the staged bases); recreate it
[FAILOVER] Adopted partition 6 from dead node 2
[FAILOVER] recovery for dead node 2: rc=0 (OK - adopted)
[ENV_REPLICA] PROMOTED replica (partition 6, index 1): 1970 page(s), capture seq 0, provenance node 2 -- staged for the migration restore path.
[FAILOVER] environment index 1 (partition 6, 3 region(s)) was NOT restored -- its replica staged on this survivor but the replay refused it (fresh environment landed off the staged bases); recreate it
[ENV_REPLICA] PROMOTED replica (partition 7, index 1): 1970 page(s), capture seq 0, provenance node 2 -- staged for the migration restore path.
[FAILOVER] environment index 1 (partition 7, 3 region(s)) was NOT restored -- its replica staged on this survivor but the replay refused it (fresh environment landed off the staged bases); recreate it
EOF

# The promotion is AFTER the refusal here: the helper must reject it rather
# than accepting the two strings in either order.
cat > "$BAD" <<'EOF'
[FAILOVER] Adopted partition 6 from dead node 2
[FAILOVER] environment index 1 (partition 6, 3 region(s)) was NOT restored -- its replica staged on this survivor but the replay refused it (fresh environment landed off the staged bases); recreate it
[ENV_REPLICA] PROMOTED replica (partition 6, index 1): 1970 page(s), capture seq 0, provenance node 2 -- staged for the migration restore path.
EOF

# Load the production classifier from the guard, not a copy.
CLASSIFIER="$(sed -n '/^restore_refused_line()/,/^}/p' "$GUARD")"
if [ -z "$CLASSIFIER" ]; then
    echo "FAIL: restore_refused_line() is missing from $GUARD"
    exit 1
fi
eval "$CLASSIFIER"
if ! declare -F restore_refused_line >/dev/null 2>&1; then
    echo "FAIL: the extracted restore_refused_line() did not define a function"
    exit 1
fi

failed=0
ok()   { echo "ok:   $1"; }
fail() { echo "FAIL: $1"; failed=1; }

if line="$(restore_refused_line "$FIXTURE" 6 3)"; then
    [ "$line" = "6" ] && ok "promotion before refusal after the adoption classifies restore-refused (line 6)" \
                      || fail "expected line 6, got '${line:-<empty>}'"
else
    fail "fixture did not classify as restore-refused"
fi

if line="$(restore_refused_line "$FIXTURE" 7 3)"; then
    [ "$line" = "8" ] && ok "the classifier is partition-scoped (partition 7 -> line 8)" \
                      || fail "expected line 8 for partition 7, got '${line:-<empty>}'"
else
    fail "partition 7 fixture did not classify"
fi

if restore_refused_line "$FIXTURE" 6 6 >/dev/null; then
    fail "a refusal at adopt_line+3 still matched when adopt_line was 6"
else
    ok "both evidence lines must be strictly after the adoption"
fi

if restore_refused_line "$BAD" 6 1 >/dev/null; then
    fail "a promotion after the refusal still classified"
else
    ok "the promotion must come before the refusal"
fi

if line="$(restore_refused_line "$FIXTURE" 6 3)"; then
    refused_text="$(sed -n "${line}p" "$FIXTURE")"
    case "$refused_text" in
        *"fresh environment landed off the staged bases"*)
            ok "the matched line carries the exact placement reason" ;;
        *)
            fail "the matched line lost the placement reason: $refused_text" ;;
    esac
else
    fail "could not re-read the matched refusal line"
fi

if grep -Fq 'restore_refused_line "$CAN_LOG" "$PART_ID2" "$adopt_line"' "$GUARD"; then
    ok "the guard calls the production classifier at the adoption site"
else
    fail "the guard no longer calls restore_refused_line at the adoption site"
fi

classify_line="$(grep -n 'env_outcome="restore-refused"; break' "$GUARD" | tail -n1 | cut -d: -f1)"
fallback_line="$(grep -n 'never named and' "$GUARD" | head -n1 | cut -d: -f1)"
if [ -n "$classify_line" ] && [ -n "$fallback_line" ] \
   && [ "$classify_line" -lt "$fallback_line" ]; then
    ok "restore-refused is classified before the 'never named' fallback"
else
    fail "restore-refused is missing or now sits after the 'never named' fallback (classify=${classify_line:-missing}, fallback=${fallback_line:-missing})"
fi

if grep -Fq 'migration replay REFUSED it' "$GUARD" \
   && grep -Fq 'placement failure' "$GUARD"; then
    ok "the failure message names the refused replay and the placement failure"
else
    fail "the failure message no longer names the refused replay and placement failure"
fi

echo
if [ "$failed" -eq 0 ]; then
    echo "failover_refused_classification_check: all claims hold"
    exit 0
fi
echo "failover_refused_classification_check: FAILED"
exit 1
