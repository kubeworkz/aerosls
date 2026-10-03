#!/usr/bin/env bash
# tests/env_storage_durable_check_smoke.sh — the TEETH for
# tests/env_storage_durable_check.sh, and the four teeth v0.2 §5 names:
#
#   P1B_TOOTH=ram-backed          — the store loses its extent (durable flag
#                                   off) but keeps its directory entry
#   P1B_TOOTH=unquotaed           — the entry's charge partition becomes
#                                   PARTITION_SYSTEM (E4 Finding 1's exact
#                                   shape on the durable side)
#   P1B_TOOTH=format-v1-only      — the v2 ceiling becomes the v1 ceiling
#                                   (the derived MAX_BLOCKS_V2 expression
#                                   replaced by v1's)
#   P1B_TOOTH=bad-format-version  — the formatting mount treats any
#                                   unparsable store as fresh, so a store
#                                   labelled v3 would be FORMATTED OVER
#
# ─── What a teeth smoke is for, and why this one is source-level ──────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE property of the durable-storage path — in a
# throwaway copy of the tree for the source half, in a synthesized recorded
# run for the replay half — and requires the guard to (a) exit 1 and
# (b) name THAT clause. Requiring the right named clause is the half that
# matters: a guard that reddened on everything, or on the wrong clause, would
# pass a tooth that only checked the exit code.
#
# The guard has TWO halves this smoke exercises, plus its controls:
#
#   * the SOURCE clauses (S1-S12), proven by mutating a throwaway copy of
#     the tree that contains exactly the files the guard reads — plus the
#     ten-file include closure its S11 host build needs, because S11 builds
#     INSIDE the inspected root (aerofs_v2_check.sh's cargo rule). So the
#     source teeth are proven on every push with no QEMU, no ISO, no build:
#     only gcc.
#   * the BOOT arm's validation (`--replay`), proven against a synthesized
#     artifact set shaped exactly like what a live run records (identity,
#     boot1/boot2 serial slices, the two console fingerprints, the four
#     quota numbers, the refusal's own serial slice). The ram-backed,
#     unquotaed and format-v1-only teeth each get a replay fixture with
#     exactly one shape of damage, so D2c/D3/D4/D5 are each proven to bite
#     without a boot. bad-format-version is source-only by nature: its
#     mutation lives in the mount's source, not in any recorded artifact.
#
# The hermetic seam for the source half is the guard's own optional root
# argument: it inspects a repository root, defaulting to its own parent. So
# this smoke builds a minimal root containing exactly the files the guard
# reads, applies one mutation, and runs the real guard against it — no
# network, no NVMe, no boot.
#
# The last arm is the vacuity control: the guard must be GREEN on the real,
# unmutated tree, and the validator GREEN on the well-formed fixture.
# Without them, a guard that failed unconditionally would pass every tooth
# in this file.
#
# Usage:  bash tests/env_storage_durable_check_smoke.sh [P1B_TOOTH=<name>]
#         P1B_TOOTH=<name> bash tests/env_storage_durable_check_smoke.sh
#         (default: all four groups, in the order above)
#
# Exit: 0 every tooth bit, 1 a tooth did not, 2 misuse/precondition missing.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/env_storage_durable_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

TOOTH_SET="${1:-${P1B_TOOTH:-}}"
case "$TOOTH_SET" in
    ""|ram-backed|unquotaed|format-v1-only|bad-format-version) ;;
    *) echo "ABORT: unknown P1B_TOOTH='$TOOTH_SET' — expected ram-backed, unquotaed, format-v1-only or bad-format-version" >&2; exit 2 ;;
esac
want_tooth() { [ -z "$TOOTH_SET" ] || [ "$TOOTH_SET" = "$1" ]; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads, PLUS the include closure of
# tests/env_storage_host_test.c (gcc -MM's own answer — S11 builds inside
# the seeded root, so a seed missing one of these would redden S11c on
# every tooth and dilute the attribution). Kept in one list so a tooth can
# never pass because the copy it mutated was missing something the guard
# needed.
SEED_FILES=(
    kernel/env_storage.h
    kernel/env_storage.c
    kernel/storage_quota.h
    kernel/storage_quota.c
    kernel/persist.h
    kernel/persist.c
    kernel/env_ckpt.c
    kernel/env_ckpt.h
    kernel/stream.h
    kernel/rowstore.h
    kernel/env_proto.h
    kernel/syscall_dispatch.c
    kernel/cap.c
    kernel/partition.h
    kernel/kernel_io.h
    drivers/nvme_io.h
    drivers/nvme_admin.h
    user/init/src/env_manager.rs
    user/ramdisk/src/server.rs
    user/proto/src/kabi.rs
    user/proto/src/lib.rs
    user/proto/src/env_proto.rs
    user/vfs/src/errno.rs
    user/vfs/src/vfs.rs
    user/vfs/src/aerofs.rs
    user/sidecar/src/boot.rs
    user/shell.c
    tests/env_storage_host_test.c
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1"
    local f
    for f in "${SEED_FILES[@]}"; do
        mkdir -p "$1/$(dirname "$f")"
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    return 0
}

replace() {   # replace <file> <old> <new> — python, so long lines need no escaping
    # ALL occurrences, not the first: a rule stated in a header and a doc
    # comment is still the rule, and replacing one copy would leave the tooth
    # biting nothing while looking aimed correctly. A pattern that is not
    # found is fatal — a tooth whose mutation did not apply would be asserted
    # against the unmutated tree and pass by accident, which is the one
    # failure mode this whole file exists to prevent, one level up.
    python3 - "$1" "$2" "$3" <<'PY' || { echo "ABORT: a tooth's mutation did not apply — aborting rather than asserting against an unmutated tree" >&2; exit 2; }
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if old not in s:
    print(f"tooth pattern not found in {p}: {old!r}", file=sys.stderr)
    sys.exit(2)
open(p, "w").write(s.replace(old, new))
PY
}

passed=0
failed=0
skipped=0
tooth() {   # tooth <P1B_TOOTH-name> <expected-clause> <label> <dir>
    local group="$1" clause="$2" label="$3" dir="$4" out rc
    out="$(bash "$GUARD" "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   P1B_TOOTH=$group: $label (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: P1B_TOOTH=$group: $label did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

replay_tooth() {   # replay_tooth <P1B_TOOTH-name> <expected-clause> <label> <dir>
    local group="$1" clause="$2" label="$3" dir="$4" out rc
    out="$(bash "$GUARD" --replay "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   P1B_TOOTH=$group: $label (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: P1B_TOOTH=$group: $label did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo "env_storage_durable_check_smoke — teeth for the durable-storage guard"
echo "======================================================================"
echo

# ── A. A missing file is a refusal to evaluate, not a pass or a failure ────
# (Always runs: every tooth group depends on the guard being evaluable at all.)
seed "$W/a"; rm -f "$W/a/kernel/env_storage.c"
out="$(bash "$GUARD" "$W/a" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT: missing "; then
    echo "ok:   A. a missing kernel/env_storage.c aborts (exit 2) instead of reporting the durable region sound"
    passed=$((passed + 1))
else
    echo "FAIL: A. a missing file did not abort (exit $rc)"; echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── P1B_TOOTH=ram-backed ──────────────────────────────────────────────────
# The §5 tooth's single mutation point: the store still exists and is still
# metered, but nothing about it reaches NVMe. Source side, the attach must
# stop marking the store DURABLE — the one flag every later branch reads.
if want_tooth ram-backed; then
    seed "$W/r1"
    replace "$W/r1/kernel/env_storage.c" \
        'ne->flags        = ENV_STORAGE_F_VALID | ENV_STORAGE_F_DURABLE;' \
        'ne->flags        = ENV_STORAGE_F_VALID;'
    tooth ram-backed S9 "attach no longer marks the store DURABLE (the flag the restore/write branches read)" "$W/r1"

    # …and the flag must actually BRANCH the restore: with it consulted only
    # at attach time, a RAM-backed store would be read back from a slot that
    # was never written.
    seed "$W/r2"
    replace "$W/r2/kernel/env_storage.c" \
        'if (!(e->flags & ENV_STORAGE_F_DURABLE) || !es_io_ready())' \
        'if (!es_io_ready())'
    tooth ram-backed S9b "restore no longer branches on the durable flag (a RAM-backed store is read from a slot nobody wrote)" "$W/r2"
else
    skipped=$((skipped + 1))
fi

# ── P1B_TOOTH=unquotaed ───────────────────────────────────────────────────
# E4 Finding 1's exact shape on the durable side: the pages are still
# charged, just to someone else. The tenant's usage never moves, so the
# over-quota refusal never fires for the tenant — metering lies quietly.
if want_tooth unquotaed; then
    seed "$W/u1"
    replace "$W/u1/kernel/env_storage.c" \
        'if (storage_page_reserve(e->partition_id)) {' \
        'if (storage_page_reserve(PARTITION_SYSTEM)) {'
    tooth unquotaed S5 "first-touch charging reserves against PARTITION_SYSTEM, not the store's own tenant" "$W/u1"
else
    skipped=$((skipped + 1))
fi

# ── P1B_TOOTH=format-v1-only ──────────────────────────────────────────────
# The v2 ceiling becomes v1's. The guard's arithmetic clause (S8) folds the
# RAW constants and would not see this mutation — S8h pins the DERIVED
# expression the mount and allocator actually consult, which is why this
# tooth exists as a lesson rather than as a comment.
if want_tooth format-v1-only; then
    seed "$W/f1"
    replace "$W/f1/user/vfs/src/aerofs.rs" \
        'pub const MAX_BLOCKS_V2: u64 = (NDIRECT_V2 + 2 * NINDIRECT) as u64;' \
        'pub const MAX_BLOCKS_V2: u64 = (NDIRECT + NINDIRECT) as u64;'
    tooth format-v1-only S8h "MAX_BLOCKS_V2 is no longer the derived v2 expression (the ceiling silently becomes v1's)" "$W/f1"
else
    skipped=$((skipped + 1))
fi

# ── P1B_TOOTH=bad-format-version ──────────────────────────────────────────
# The formatting mount treats any unparsable store as fresh. Its refusal is
# two gates: consult the refusing parser, and detect freshness by block 0's
# zero bytes. This mutation removes the SECOND gate — the parser call stays
# textually present but its branch is unreachable, so a string check on the
# parser alone would pass a mount that formats v3 over. S8e requires both.
if want_tooth bad-format-version; then
    seed "$W/b1"
    replace "$W/b1/user/vfs/src/vfs.rs" \
        'if block.iter().all(|&b| b == 0) {' \
        'if true {'
    tooth bad-format-version S8e "the mount's zero-gate is gone (every unparsable store is 'unformatted' and gets formatted over)" "$W/b1"
else
    skipped=$((skipped + 1))
fi

# ═══ The boot arm's teeth: the artifact validation half, no QEMU ══════════
# The set below is what a live run records (the guard's own file names and
# shapes); a mutation here is the same class of mistake as a wrong boot, and
# the guard must name the clause that catches it.
write_arts() {   # write_arts <dir> — a well-formed recorded run
    local d="$1"
    rm -rf "$d"; mkdir -p "$d"
    cat > "$d/identity.json" <<'JSON'
{"partition": 3, "index": 2, "env_id": 4242, "partition_name": "p1bstore"}
JSON
    cat > "$d/create.json" <<'JSON'
{"ok":"true","env_id":4242,"index":2}
JSON
    cat > "$d/restore.json" <<'JSON'
{"ok":"true","pending":1,"replayed":1,"refused":0,"remaining":0}
JSON
    cat > "$d/envlist.json" <<'JSON'
{"ok":"true","partition":3,"envs":[{"env_id":5001,"index":2,"posix_pid":77}],"live":1}
JSON
    # The serial slices, line for line what the kernel and persist.c print
    # (env_storage.c's attach/restore, persist.c's writer/restore).
    cat > "$d/boot1.log" <<'LOG'
[BOOT] command line: unified=1 node=0
[ENV-STORAGE] attach partition=3 index=2 base=4194304 bytes=1048576 slot=0 durable=1 charged=0 (new store)
[PERSIST] Environment storage directory written (1 store(s)).
LOG
    cat > "$d/boot2.log" <<'LOG'
[BOOT] command line: unified=1 node=0
[PERSIST] Environment storage directory restored (1 store(s)).
[ENV-STORAGE] restore partition=3 index=2 slot=0 pages=34 loaded from LBA 1114112
LOG
    # The big file's fingerprint, in the two-part shape the live arm records:
    # wc= line + head -n 20 block, then the tail -n 20 block. pre and post
    # are byte-equal — the claim D3a makes.
    cat > "$d/big_pre.txt" <<'FPR'
wc=128894
-- head -n 20
1
2
3
4
5
6
7
8
9
10
11
12
13
14
15
16
17
18
19
20
-- tail -n 20
19981
19982
19983
19984
19985
19986
19987
19988
19989
19990
19991
19992
19993
19994
19995
19996
19997
19998
19999
20000
FPR
    cp "$d/big_pre.txt" "$d/big_post.txt"
    printf 'p1b-small-payload\n' > "$d/small_pre.txt"
    cp "$d/small_pre.txt" "$d/small_post.txt"
    # The quota's four numbers, as the live arm records them: usage before
    # (0 — a new store charges on first touch), after (the 128 894-byte big
    # file ≈ 31 pages + the small file's 1 = 33), after the reboot (the
    # directory re-charged the same 33), and the over-quota window's own
    # three (ceiling = reboot + 4, usage pinned AT the ceiling, the file
    # refused after four more pages).
    printf '0\n'  > "$d/quota_pages_before.txt"
    printf '33\n' > "$d/quota_pages_after.txt"
    printf '33\n' > "$d/quota_pages_reboot.txt"
    printf '1\n'  > "$d/quota_denied.txt"
    printf '37\n' > "$d/quota_ceiling.txt"
    printf '37\n' > "$d/quota_usage_end.txt"
    printf '0\n'  > "$d/frame_denied.txt"
    printf '16384\n' > "$d/big2_wc.txt"
}

echo
# ── The vacuity control for the validator: a well-formed run passes ───────
write_arts "$W/art-good"
out="$(bash "$GUARD" --replay "$W/art-good" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   the validator passes a well-formed recorded run ($(printf '%s\n' "$out" | grep -c '^ok:' ) clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: the validator REJECTED a well-formed recorded run — it is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── P1B_TOOTH=ram-backed: the replay half ─────────────────────────────────
# What the tooth leaves behind: the entry (so the directory reloaded and the
# re-charge happened), but the extent was never read back — restore answers
# "RAM-backed, nothing to load", the files' bytes are gone after the
# reboot, and the QUOTA's evidence is untouched: the refusal still fired,
# the usage still pinned at the ceiling. D4 and D5 staying green while
# D2c/D3 go red is the control that separates "durable" from "was
# written" — durability and metering proved independent of each other.
if want_tooth ram-backed; then
    cp -a "$W/art-good" "$W/art-ram"
    python3 - "$W/art-ram" <<'PY'
import sys, re, os
d = sys.argv[1]
b2 = open(os.path.join(d, "boot2.log")).read()
# The RAM-backed restore's own line, in place of the load-from-NVMe one:
b2 = re.sub(r"\[ENV-STORAGE\] restore partition=3 index=2 [^\n]*loaded from LBA [^\n]*\n",
            "[ENV-STORAGE] restore slot=0: RAM-backed, nothing to load\n", b2)
open(os.path.join(d, "boot2.log"), "w").write(b2)
# The files did not come back: both fingerprints differ post-reboot.
open(os.path.join(d, "big_post.txt"), "w").write("wc=0\n-- head -n 20\n(nothing: the store was RAM-backed)\n")
open(os.path.join(d, "small_post.txt"), "w").write("")
# The quota numbers are NOT touched: the entry persisted, so the re-charge
# and the refusal are exactly as recorded.
PY
    printf 'ram-backed\n' > "$W/art-ram/tooth.txt"
    out="$(bash "$GUARD" --replay "$W/art-ram" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: D2c\." && \
       printf '%s\n' "$out" | grep -q "^FAIL: D3a\." && \
       printf '%s\n' "$out" | grep -q "^ok:   D4\." && \
       printf '%s\n' "$out" | grep -q "^ok:   D5\."; then
        echo "ok:   P1B_TOOTH=ram-backed: the replay takes D2c (no LBA restore line) and D3a (bytes gone) red while D4/D5 hold green — durability reddens alone"
        passed=$((passed + 1))
    else
        echo "FAIL: P1B_TOOTH=ram-backed did NOT bite as attributed — exit $rc (want 1, D2c+D3a red, D4+D5 green)"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
else
    skipped=$((skipped + 1))
fi

# ── P1B_TOOTH=unquotaed: the replay half ──────────────────────────────────
# The tenant's usage never moves (it was charged to PARTITION_SYSTEM), so
# there is nothing to re-charge after the reboot and nothing ever reaches
# the ceiling — no refusal line, and the over-quota file lands in full.
# Every durability-side clause stays green: the bytes are fine, it is the
# METERING that lies.
if want_tooth unquotaed; then
    cp -a "$W/art-good" "$W/art-unq"
    printf '0\n'  > "$W/art-unq/quota_pages_after.txt"
    printf '0\n'  > "$W/art-unq/quota_pages_reboot.txt"
    printf '0\n'  > "$W/art-unq/quota_denied.txt"
    printf '0\n'  > "$W/art-unq/quota_usage_end.txt"
    printf '138894\n' > "$W/art-unq/big2_wc.txt"
    printf 'unquotaed\n' > "$W/art-unq/tooth.txt"
    out="$(bash "$GUARD" --replay "$W/art-unq" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: D4\." && \
       printf '%s\n' "$out" | grep -q "^ok:   D3a\."; then
        echo "ok:   P1B_TOOTH=unquotaed: the replay takes D4 (the tenant's usage never moves) red while D3a holds green — metering reddens alone"
        passed=$((passed + 1))
    else
        echo "FAIL: P1B_TOOTH=unquotaed did NOT bite as attributed — exit $rc (want 1, D4 red, D3a green)"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
else
    skipped=$((skipped + 1))
fi

# ── P1B_TOOTH=format-v1-only: the replay half ─────────────────────────────
# The big file never reached 71 200 bytes before the reboot (the live arm
# records exactly this marker when wc -c comes back short), the small file
# is untouched, and the quota moved only by what actually landed. D3a must
# go red on the marker; D3b must stay green — the split that names the
# LARGE file's clause as the one the ceiling broke.
if want_tooth format-v1-only; then
    cp -a "$W/art-good" "$W/art-v1"
    printf 'BIGWRITE-TOO-SMALL 69632\n' > "$W/art-v1/big_pre.txt"
    printf 'BIGWRITE-TOO-SMALL 69632\n' > "$W/art-v1/big_post.txt"
    printf '17\n' > "$W/art-v1/quota_pages_after.txt"
    printf '17\n' > "$W/art-v1/quota_pages_reboot.txt"
    printf '21\n' > "$W/art-v1/quota_ceiling.txt"
    printf '21\n' > "$W/art-v1/quota_usage_end.txt"
    printf 'format-v1-only\n' > "$W/art-v1/tooth.txt"
    replay_tooth format-v1-only D3a "the replay takes D3a (the big file never landed) red — the v1 ceiling refused it" "$W/art-v1"
    out="$(bash "$GUARD" --replay "$W/art-v1" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^ok:   D3b\."; then
        echo "ok:   P1B_TOOTH=format-v1-only: D3b (the small file) holds green — the refusal is the LARGE file's clause alone"
        passed=$((passed + 1))
    else
        echo "FAIL: P1B_TOOTH=format-v1-only: D3b did NOT hold green — exit $rc"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
else
    skipped=$((skipped + 1))
fi

# bad-format-version has no replay fixture on purpose: its mutation lives
# in the mount's SOURCE (the S8e arm above), and no recorded artifact of a
# run that formatted v3 over differs from one that refused it — the refusal
# is a pre-boot decision. The live arm's own evidence for it is serial-side
# (boot.rs's MountRefused line), asserted by S8g.

# ── The vacuity control ───────────────────────────────────────────────────
# The guard must be GREEN on the untouched tree. Without this arm, a guard
# that failed unconditionally would pass every tooth above.
out="$(bash "$GUARD" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   control: the untouched tree passes ($(printf '%s\n' "$out" | grep -c '^ok:' ) clauses green)"
    passed=$((passed + 1))
else
    echo "FAIL: control: the untouched tree does NOT pass — the guard is not measuring what it claims"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
[ "$skipped" -gt 0 ] && echo "($skipped tooth group(s) not selected by P1B_TOOTH=$TOOTH_SET)"
echo "$passed passed, $failed failed"
[ "$failed" -eq 0 ]
