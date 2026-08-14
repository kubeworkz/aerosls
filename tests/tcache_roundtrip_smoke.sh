#!/usr/bin/env bash
# tests/tcache_roundtrip_smoke.sh — proves the round-trip guard's teeth bite,
# by synthesizing the recorded artifacts and asserting the guard fails when
# each one is mutated.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/tcache_roundtrip_check.sh boots the ISO, sweep-benches (loads
# 1,64,128,256,500 -> block counts 1,2,3,5,8), checkpoints, reboots, and
# asserts the Phase 2 cache round-tripped (cold compiled, warm all-hit,
# identical insns) PER VALUE. Its teeth were proven by one hand-run
# measurement. Nothing made it fail automatically, so a regression that made
# the guard blind — a renamed JSON field, a dropped grep, a validation that
# only ever reads the first artifact — would pass every push unnoticed while
# claiming the round-trip works.
#
# ─── Why it is source-only ─────────────────────────────────────────────────
# The guard's live half needs a built ISO and QEMU, so the smoke cannot
# re-run it here. Instead the smoke uses the guard's --replay mode: the
# validation half is identical whether the artifacts were produced by a live
# boot or synthesized. A well-formed artifact set is synthesized, the guard
# must ACCEPT it (tooth 0 — proves the true shape is the shape the guard
# expects), then each assertion input is mutated and the guard must FAIL on
# it for the right reason. The live half is exercised on every push by the
# guard itself (kernel-guards CI, GUARD-KIND: build) — this smoke proves the
# guard cannot go blind while it is running there.
#
# ─── The teeth ─────────────────────────────────────────────────────────────
# The artifact values mirror the sweep shape: five loads values at their own
# GPAs (0x10000 * (i+1)), block counts 1,2,3,5,8. Each tooth breaks ONE
# assertion input:
#
#   1.  warm value 0 hits 0            -> the per-value "all hit" contract
#   2.  warm value 1 blocks 3          -> the "0 blocks compiled" contract
#   3.  warm value 2 cold=true         -> the "warm run must be warm" contract
#   4.  warm value 3 insns changed     -> the "identical result" contract
#   5.  cold value 4 blocks 0          -> the "each value compiled >= 1" tooth
#   6.  cold value 0 cold=false        -> the cold-start tooth
#   7.  cold value 1 insns != loads+2  -> the "program completed" tooth
#   8.  cold value 2 hits 3            -> the "cold compiles everything" tooth
#   9.  cold blocks not increasing     -> the "multiple block counts" tooth
#   10. boot.log without "warm start"  -> the restore-banner grep
#   11. boot.log "NVMe unavailable"    -> the >4 GiB BAR degradation grep
#   12. checkpoint status 2            -> the sync-status tooth
#   13. warm sweep missing a value     -> the shape-match tooth (length)
#   14. cold sweep with a stray value  -> the shape-match tooth (length)
#
# Exit: 0 if every tooth bit, 1 otherwise, 2 if python3 is missing.
set -u
cd "$(dirname "$0")/.."   # repo root
guard=tests/tcache_roundtrip_check.sh
fails=0

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }

TD="$(mktemp -d)"
trap 'rm -rf "$TD"' EXIT

# ─── The well-formed artifact set (mirrors the sweep shape) ────────────────
# One JSON object per line so a tooth can sed one value without touching the
# others; JSON does not care about the whitespace.
make_artifacts() {   # $1 = directory
    mkdir -p "$1"
    cat > "$1/cold_sweep.json" <<'EOF'
{"ok":"true","count":5,"results":[
 {"ok":"true","loads":1,"gpa":65536,"insns":3,"blocks":1,"code_bytes":1024,"tcache_hits":0,"tcache_misses":1,"cold":"true","arena_consumed":100000,"arena_used":100000,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":64,"gpa":131072,"insns":66,"blocks":2,"code_bytes":2048,"tcache_hits":0,"tcache_misses":2,"cold":"true","arena_consumed":110000,"arena_used":210000,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":128,"gpa":196608,"insns":130,"blocks":3,"code_bytes":3072,"tcache_hits":0,"tcache_misses":3,"cold":"true","arena_consumed":120000,"arena_used":330000,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":256,"gpa":262144,"insns":258,"blocks":5,"code_bytes":5120,"tcache_hits":0,"tcache_misses":5,"cold":"true","arena_consumed":130000,"arena_used":460000,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":500,"gpa":327680,"insns":502,"blocks":8,"code_bytes":8105,"tcache_hits":0,"tcache_misses":8,"cold":"true","arena_consumed":1507392,"arena_used":1508448,"arena_total":67108864,"softmmu":"off"}
]}
EOF
    cat > "$1/warm_sweep.json" <<'EOF'
{"ok":"true","count":5,"results":[
 {"ok":"true","loads":1,"gpa":65536,"insns":3,"blocks":0,"code_bytes":0,"tcache_hits":1,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":64,"gpa":131072,"insns":66,"blocks":0,"code_bytes":0,"tcache_hits":2,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":128,"gpa":196608,"insns":130,"blocks":0,"code_bytes":0,"tcache_hits":3,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":256,"gpa":262144,"insns":258,"blocks":0,"code_bytes":0,"tcache_hits":5,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"},
 {"ok":"true","loads":500,"gpa":327680,"insns":502,"blocks":0,"code_bytes":0,"tcache_hits":8,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"}
]}
EOF
    printf '%s\n' \
        '[QEMU-SLS TCACHE] no snapshot — cold start' \
        '[QEMU-SLS TCACHE] synced: 19 TBs, 19477 code bytes (buffer high-water, includes inter-block alignment)' \
        '[QEMU-SLS TCACHE] warm start — codebuf_used=19477, codebuf=0x0000000007ae0000' > "$1/boot.log"
    printf '%s' '{"status":0,"seq":1,"count":19}' > "$1/checkpoint.json"
}

# tooth NAME  -> runs the guard against the current artifacts
tooth() {   # $1 = tooth name, $2 = expected failure substring
    local name="$1" expect="$2"
    local out rc
    out="$(bash "$guard" --replay "$TD/art" 2>&1)"
    rc=$?
    if [ "$rc" -ne 1 ]; then
        echo "TOOTH $name FAIL  guard exited $rc (expected 1) on the mutated set:"
        printf '%s\n' "$out" | sed 's/^/      /'
        fails=$((fails + 1))
        return
    fi
    if ! printf '%s' "$out" | grep -q "$expect"; then
        echo "TOOTH $name FAIL  guard failed, but not for the expected reason (want: $expect):"
        printf '%s\n' "$out" | sed 's/^/      /'
        fails=$((fails + 1))
        return
    fi
    echo "TOOTH $name OK   ($expect)"
}

# ─── Tooth 0: the true shape must PASS (a broken tooth can never leave the
# tree looking failed -- the guard must accept what the measurement really
# produces before any mutation is trusted). ─────────────────────────────────
make_artifacts "$TD/art"
if out="$(bash "$guard" --replay "$TD/art" 2>&1)"; then
    echo "TOOTH 0 OK   well-formed sweep artifacts accepted"
else
    echo "TOOTH 0 FAIL  the guard rejected its own true shape:"
    printf '%s\n' "$out" | sed 's/^/      /'
    fails=$((fails + 1))
fi

# ─── Tooth 1: warm value 0 with 0 hits — its restored TBs were ignored. ────
make_artifacts "$TD/art"
sed -i 's/"tcache_hits":1/"tcache_hits":0/' "$TD/art/warm_sweep.json"
tooth "1 (warm hits=0)"     "not all hit"

# ─── Tooth 2: warm value 1 compiled 3 new blocks. ──────────────────────────
make_artifacts "$TD/art"
sed -i '3s/"blocks":0/"blocks":3/' "$TD/art/warm_sweep.json"
tooth "2 (warm blocks=3)"   "nothing may be compiled"

# ─── Tooth 3: warm value 2 reported cold=true — cache not consulted. ───────
make_artifacts "$TD/art"
sed -i '4s/"cold":"false"/"cold":"true"/' "$TD/art/warm_sweep.json"
tooth "3 (warm cold=true)"  "must hit the restored cache"

# ─── Tooth 4: warm value 3 insns differ — result changed across reboot. ────
make_artifacts "$TD/art"
sed -i '5s/"insns":258/"insns":257/' "$TD/art/warm_sweep.json"
tooth "4 (warm insns!=cold)" "differed across the reboot"

# ─── Tooth 5: cold value 4 compiled 0 blocks — the value never ran. ────────
make_artifacts "$TD/art"
sed -i '6s/"blocks":8/"blocks":0/' "$TD/art/cold_sweep.json"
tooth "5 (cold blocks=0)"   "expected >= 1 compiled"

# ─── Tooth 6: cold value 0 reported cold=false — the disk was reused. ──────
make_artifacts "$TD/art"
sed -i '2s/"cold":"true"/"cold":"false"/' "$TD/art/cold_sweep.json"
tooth "6 (cold cold=false)" "fresh cold start"

# ─── Tooth 7: cold value 1 did not complete — insns != loads+2. ────────────
make_artifacts "$TD/art"
sed -i '3s/"insns":66/"insns":65/' "$TD/art/cold_sweep.json"
tooth "7 (cold insns short)" "the bench program is"

# ─── Tooth 8: cold value 2 already hit the cache — nothing compiled. ───────
make_artifacts "$TD/art"
sed -i '4s/"tcache_hits":0/"tcache_hits":3/' "$TD/art/cold_sweep.json"
tooth "8 (cold hits=3)"     "compile everything"

# ─── Tooth 9: cold blocks not strictly increasing — single-count sweep. ────
make_artifacts "$TD/art"
sed -i '5s/"blocks":5/"blocks":3/' "$TD/art/cold_sweep.json"
tooth "9 (blocks not incr)" "span multiple block counts"

# ─── Tooth 10: the restore banner never appeared. ──────────────────────────
make_artifacts "$TD/art"
sed -i '/warm start/d' "$TD/art/boot.log"
tooth "10 (no warm banner)" "not restored from NVMe"

# ─── Tooth 11: the >4 GiB BAR degradation — the round-trip never happened. ─
make_artifacts "$TD/art"
sed -i 's/no snapshot — cold start/NVMe unavailable — cold start/' "$TD/art/boot.log"
tooth "11 (NVMe unavailable)" "BAR landed above 4 GiB"

# ─── Tooth 12: the checkpoint failed to sync. ──────────────────────────────
make_artifacts "$TD/art"
sed -i 's/"status":0/"status":2/' "$TD/art/checkpoint.json"
tooth "12 (checkpoint=2)"   "sync to NVMe"

# ─── Tooth 13: the warm sweep lost a value — shape must match. ─────────────
# Deleted value 4 and its trailing comma, so the artifact is still valid JSON
# and the guard must fail on the LENGTH check, not on a parse error.
make_artifacts "$TD/art"
sed -i -e '6d' -e '5s/,$//' "$TD/art/warm_sweep.json"
tooth "13 (warm short)"     "sweep shape changed"

# ─── Tooth 14: the cold sweep gained a stray value — shape must match. ─────
# Splices a 6th value in before the closing bracket, valid JSON, so the
# guard must fail on the LENGTH check (its per-value loop only walks the
# expected five).
make_artifacts "$TD/art"
sed -i '6s/}$/},\n {"ok":"true","loads":999,"gpa":393216,"insns":1001,"blocks":1,"code_bytes":1024,"tcache_hits":0,"tcache_misses":1,"cold":"true","arena_consumed":100000,"arena_used":100000,"arena_total":67108864,"softmmu":"off"}/' "$TD/art/cold_sweep.json"
tooth "14 (cold long)"      "sweep shape changed"

if [ "$fails" -eq 0 ]; then
    echo
    echo "PASS  tcache_roundtrip guard: 1 accept + 14 reject teeth all bite"
    exit 0
fi

echo
echo "FAIL  $fails tcache_roundtrip tooth/teeth did not bite" >&2
exit 1
