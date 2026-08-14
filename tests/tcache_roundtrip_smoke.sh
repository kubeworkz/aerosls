#!/usr/bin/env bash
# tests/tcache_roundtrip_smoke.sh — proves the round-trip guard's teeth bite,
# by synthesizing the recorded artifacts and asserting the guard fails when
# each one is mutated.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/tcache_roundtrip_check.sh boots the ISO, benches, checkpoints,
# reboots, and asserts the Phase 2 cache round-tripped (cold compiled, warm
# all-hit, identical insns). Its teeth were proven by one hand-run
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
# The artifact values mirror the iteration-32 measurement exactly (8 blocks,
# 502 insns, 8174 code bytes synced). Each tooth breaks ONE assertion input:
#
#   1. warm hits 0            -> the "8/8 hits" contract
#   2. warm blocks 3          -> the "0 blocks compiled" contract
#   3. warm cold=true         -> the "warm run must be a warm run" contract
#   4. boot.log without "warm start"   -> the restore-banner grep
#   5. boot.log "NVMe unavailable"     -> the >4 GiB BAR degradation grep
#   6. checkpoint status 2    -> the sync-status tooth
#   7. cold cold=false        -> the cold-start tooth
#   8. cold hits 8            -> the "cold compiles everything" tooth
#
# Exit: 0 if every tooth bit, 1 otherwise, 2 if python3 is missing.
set -u
cd "$(dirname "$0")/.."   # repo root
guard=tests/tcache_roundtrip_check.sh
fails=0

command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not found" >&2; exit 2; }

TD="$(mktemp -d)"
trap 'rm -rf "$TD"' EXIT

# ─── The well-formed artifact set (mirrors the iteration-32 measurement) ───
make_artifacts() {   # $1 = directory
    mkdir -p "$1"
    printf '%s' '{"ok":"true","loads":500,"insns":502,"blocks":8,"code_bytes":8105,"tcache_hits":0,"tcache_misses":8,"cold":"true","arena_consumed":1507392,"arena_used":1508448,"arena_total":67108864,"softmmu":"off"}' > "$1/cold.json"
    printf '%s' '{"status":0,"seq":1,"count":1}' > "$1/checkpoint.json"
    printf '%s\n' \
        '[QEMU-SLS TCACHE] no snapshot — cold start' \
        '[QEMU-SLS TCACHE] synced: 8 TBs, 8174 code bytes (buffer high-water, includes inter-block alignment)' \
        '[QEMU-SLS TCACHE] warm start — codebuf_used=8174, codebuf=0x0000000007ae0000' > "$1/boot.log"
    printf '%s' '{"ok":"true","loads":500,"insns":502,"blocks":0,"code_bytes":0,"tcache_hits":8,"tcache_misses":0,"cold":"false","arena_consumed":0,"arena_used":1056,"arena_total":67108864,"softmmu":"off"}' > "$1/warm.json"
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
    echo "TOOTH 0 OK   well-formed artifacts accepted"
else
    echo "TOOTH 0 FAIL  the guard rejected its own true shape:"
    printf '%s\n' "$out" | sed 's/^/      /'
    fails=$((fails + 1))
fi

# ─── Tooth 1: warm run with 0 hits — the restored TBs were ignored. ────────
make_artifacts "$TD/art"
sed -i 's/"tcache_hits":8/"tcache_hits":0/' "$TD/art/warm.json"
tooth "1 (warm hits=0)"      "not all hit"

# ─── Tooth 2: warm run compiled 3 new blocks. ──────────────────────────────
make_artifacts "$TD/art"
sed -i 's/"blocks":0/"blocks":3/' "$TD/art/warm.json"
tooth "2 (warm blocks=3)"    "nothing may be compiled"

# ─── Tooth 3: warm run reported cold=true — the cache was not consulted. ───
make_artifacts "$TD/art"
sed -i 's/"cold":"false"/"cold":"true"/' "$TD/art/warm.json"
tooth "3 (warm cold=true)"   "must hit the restored cache"

# ─── Tooth 4: the restore banner never appeared. ───────────────────────────
make_artifacts "$TD/art"
sed -i '/warm start/d' "$TD/art/boot.log"
tooth "4 (no warm-start banner)" "not restored from NVMe"

# ─── Tooth 5: the >4 GiB BAR degradation — the round-trip never happened. ──
make_artifacts "$TD/art"
sed -i 's/no snapshot — cold start/NVMe unavailable — cold start/' "$TD/art/boot.log"
tooth "5 (NVMe unavailable)" "BAR landed above 4 GiB"

# ─── Tooth 6: the checkpoint failed to sync. ───────────────────────────────
make_artifacts "$TD/art"
sed -i 's/"status":0/"status":2/' "$TD/art/checkpoint.json"
tooth "6 (checkpoint status=2)" "sync to NVMe"

# ─── Tooth 7: boot 1 was not a cold start — the disk was reused. ───────────
make_artifacts "$TD/art"
sed -i 's/"cold":"true"/"cold":"false"/' "$TD/art/cold.json"
tooth "7 (cold cold=false)"  "fresh cold start"

# ─── Tooth 8: boot 1 already hit the cache — nothing was compiled. ─────────
make_artifacts "$TD/art"
sed -i 's/"tcache_hits":0/"tcache_hits":8/' "$TD/art/cold.json"
tooth "8 (cold hits=8)"      "compile everything"

if [ "$fails" -eq 0 ]; then
    echo
    echo "PASS  tcache_roundtrip guard: 1 accept + 8 reject teeth all bite"
    exit 0
fi

echo
echo "FAIL  $fails tcache_roundtrip tooth/teeth did not bite" >&2
exit 1
