#!/usr/bin/env bash
# tests/tcache_roundtrip_smoke.sh — proves the round-trip guard's teeth bite,
# by synthesizing the recorded artifacts and asserting the guard fails when
# each one is mutated.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/tcache_roundtrip_check.sh boots the ISO, sweep-benches (sixteen
# loads values -> block counts 1..11 then 13..17), checkpoints, reboots, and
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
# The artifact values mirror the measured sweep shape: sixteen loads values
# (one per block count 1..11 then 13..17) at their own 128 KiB-aligned GPAs
# (0x20000 * (i+1)). Each tooth breaks ONE assertion input:
#
#   1.  warm value 1 hits 0            -> the per-value "all hit" contract
#   2.  warm value 2 blocks 3          -> the "0 blocks compiled" contract
#   3.  warm value 3 cold=true         -> the "warm run must be warm" contract
#   4.  warm value 4 insns changed     -> the "identical result" contract
#   5.  cold value 16 blocks 0         -> the "each value compiled >= 1" tooth
#   6.  cold value 1 cold=false        -> the cold-start tooth
#   7.  cold value 2 insns != loads+2  -> the "program completed" tooth
#   8.  cold value 3 hits 3            -> the "cold compiles everything" tooth
#   9.  cold blocks not increasing     -> the "multiple block counts" tooth
#   10. boot.log without "warm start"  -> the restore-banner grep
#   11. boot.log "NVMe unavailable"    -> the >4 GiB BAR degradation grep
#   12. checkpoint status 2            -> the sync-status tooth
#   13. warm sweep missing a value     -> the shape-match tooth (length)
#   14. cold sweep with a stray value  -> the shape-match tooth (length)
#   15. a value claims 12 blocks       -> the documented unattainable-count
#                                         tooth (the page-crossing gap)
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
# others; JSON does not care about the whitespace. The loads -> blocks map
# is the measured one: loads 64k-63 -> k blocks while the program fits one
# 4 KiB page (k <= 11); a larger program always crosses the page after 682
# instructions and starts at 13 blocks, so 12 is skipped (see the guard).
# gpa = 0x20000 * (i+1); insns = loads + 2.
make_artifacts() {   # $1 = directory
    mkdir -p "$1"
    # pairs: "loads blocks"
    pairs="1 1
65 2
129 3
193 4
257 5
321 6
385 7
449 8
513 9
577 10
641 11
682 13
750 14
833 15
897 16
961 17"
    {
        echo '{"ok":"true","count":16,"results":['
        i=0
        echo "$pairs" | while read -r loads blocks; do
            i=$((i + 1))
            insns=$((loads + 2))
            gpa=$((i * 0x20000))
            if [ "$i" -lt 16 ]; then sep=','; else sep=''; fi
            if [ "$i" -eq 1 ]; then arena=1507392; else arena=0; fi
            printf ' {"ok":"true","loads":%d,"gpa":%d,"insns":%d,"blocks":%d,"code_bytes":%d,"tcache_hits":0,"tcache_misses":%d,"cold":"true","arena_consumed":%d,"arena_used":1508448,"arena_total":67108864,"softmmu":"off"}%s\n' \
                "$loads" "$gpa" "$insns" "$blocks" $((1024 * blocks)) "$blocks" "$arena" "$sep"
        done
        echo ']}'
    } > "$1/cold_sweep.json"
    {
        echo '{"ok":"true","count":16,"results":['
        i=0
        echo "$pairs" | while read -r loads blocks; do
            i=$((i + 1))
            insns=$((loads + 2))
            gpa=$((i * 0x20000))
            if [ "$i" -lt 16 ]; then sep=','; else sep=''; fi
            printf ' {"ok":"true","loads":%d,"gpa":%d,"insns":%d,"blocks":0,"code_bytes":0,"tcache_hits":%d,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"}%s\n' \
                "$loads" "$gpa" "$insns" "$blocks" "$sep"
        done
        echo ']}'
    } > "$1/warm_sweep.json"
    printf '%s\n' \
        '[QEMU-SLS TCACHE] no snapshot — cold start' \
        '[QEMU-SLS TCACHE] synced: 141 TBs, 126686 code bytes (buffer high-water, includes inter-block alignment)' \
        '[QEMU-SLS TCACHE] warm start — codebuf_used=126686, codebuf=0x0000000007ae1000' > "$1/boot.log"
    printf '%s' '{"status":0,"seq":1,"count":141}' > "$1/checkpoint.json"
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

# ─── Tooth 1: warm value 1 with 0 hits — its restored TBs were ignored. ────
# Line-targeted: a global s/ would also hit "tcache_hits":16 (value 16) and
# turn it into ":06", which is invalid JSON and would fail for the wrong
# reason.
make_artifacts "$TD/art"
sed -i '2s/"tcache_hits":1/"tcache_hits":0/' "$TD/art/warm_sweep.json"
tooth "1 (warm hits=0)"     "not all hit"

# ─── Tooth 2: warm value 2 compiled 3 new blocks. ──────────────────────────
make_artifacts "$TD/art"
sed -i '3s/"blocks":0/"blocks":3/' "$TD/art/warm_sweep.json"
tooth "2 (warm blocks=3)"   "nothing may be compiled"

# ─── Tooth 3: warm value 3 reported cold=true — cache not consulted. ───────
make_artifacts "$TD/art"
sed -i '4s/"cold":"false"/"cold":"true"/' "$TD/art/warm_sweep.json"
tooth "3 (warm cold=true)"  "must hit the restored cache"

# ─── Tooth 4: warm value 4 insns differ — result changed across reboot. ────
make_artifacts "$TD/art"
sed -i '5s/"insns":195/"insns":194/' "$TD/art/warm_sweep.json"
tooth "4 (warm insns!=cold)" "differed across the reboot"

# ─── Tooth 5: cold value 16 compiled 0 blocks — the value never ran. ───────
make_artifacts "$TD/art"
sed -i '17s/"blocks":17/"blocks":0/' "$TD/art/cold_sweep.json"
tooth "5 (cold blocks=0)"   "expected >= 1 compiled"

# ─── Tooth 6: cold value 1 reported cold=false — the disk was reused. ──────
make_artifacts "$TD/art"
sed -i '2s/"cold":"true"/"cold":"false"/' "$TD/art/cold_sweep.json"
tooth "6 (cold cold=false)" "fresh cold start"

# ─── Tooth 7: cold value 2 did not complete — insns != loads+2. ────────────
make_artifacts "$TD/art"
sed -i '3s/"insns":67/"insns":66/' "$TD/art/cold_sweep.json"
tooth "7 (cold insns short)" "the bench program is"

# ─── Tooth 8: cold value 3 already hit the cache — nothing compiled. ───────
make_artifacts "$TD/art"
sed -i '4s/"tcache_hits":0/"tcache_hits":3/' "$TD/art/cold_sweep.json"
tooth "8 (cold hits=3)"     "compile everything"

# ─── Tooth 9: cold blocks not strictly increasing — single-count sweep. ────
make_artifacts "$TD/art"
sed -i '9s/"blocks":8/"blocks":7/' "$TD/art/cold_sweep.json"
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
# Deletes the LAST value (line 17) and its trailing comma (from line 16), so
# the artifact stays valid JSON and the guard must fail on the LENGTH check,
# not on a parse error. (Deleting a MIDDLE value would leave two values
# adjacent with no comma -- a parse error, the wrong failure.)
make_artifacts "$TD/art"
sed -i -e '17d' -e '16s/,$//' "$TD/art/warm_sweep.json"
tooth "13 (warm short)"     "sweep shape changed"

# ─── Tooth 14: the cold sweep gained a stray value — shape must match. ─────
# Splices a 17th value in before the closing bracket, valid JSON, so the
# guard must fail on the LENGTH check (its per-value loop only walks the
# expected sixteen).
make_artifacts "$TD/art"
sed -i '17s/}$/},\n {"ok":"true","loads":1023,"gpa":2179072,"insns":1025,"blocks":1,"code_bytes":1024,"tcache_hits":0,"tcache_misses":1,"cold":"true","arena_consumed":100000,"arena_used":100000,"arena_total":67108864,"softmmu":"off"}/' "$TD/art/cold_sweep.json"
tooth "14 (cold long)"      "sweep shape changed"

# ─── Tooth 15: a cold value claims the unattainable 12-block count. ────────
# The documented page-crossing gap (iteration 35) says 12 blocks is
# impossible with the bench's straight-line shape; a measurement showing it
# means the sweep values and the doc are stale. Cold blocks AND warm hits
# are mutated together (line 13 = value 12) so the round-trip still agrees
# internally -- only the gap check may fire.
make_artifacts "$TD/art"
sed -i '13s/"blocks":13/"blocks":12/' "$TD/art/cold_sweep.json"
sed -i '13s/"tcache_hits":13/"tcache_hits":12/' "$TD/art/warm_sweep.json"
tooth "15 (unattainable 12)" "12 is unattainable"

if [ "$fails" -eq 0 ]; then
    echo
    echo "PASS  tcache_roundtrip guard: 1 accept + 15 reject teeth all bite"
    exit 0
fi

echo
echo "FAIL  $fails tcache_roundtrip tooth/teeth did not bite" >&2
exit 1
