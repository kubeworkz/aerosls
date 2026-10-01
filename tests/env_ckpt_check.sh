#!/usr/bin/env bash
# tests/env_ckpt_check.sh — guards the WIRING of POSIX-Environments v0.2
# Phase P1a (the environment checkpoint record), not its logic.
#
# ─── Why this guard exists, and why it is not the host test ────────────────
# tests/env_ckpt_host_test.c proves the record layer's behaviour: the refusal
# gate, the all-or-nothing adopt, the dense table. This guard proves the
# opposite kind of thing — that the behaviour is actually REACHABLE from a
# boot. A checkpoint record layer that is correct, tested, and never written
# or never read is not a feature; it is a file.
#
# That failure mode is not hypothetical in this repository. kernel/persist.c's
# own comments record two instances of exactly it, found by hand:
#
#   * the capacity-sizing pass updated persist.h's layout table in COMMENTS and
#     left twenty-four of thirty-five constants describing the old layout,
#     producing ten overlapping regions that silently corrupted each other;
#   * qemu_sls_snapshot_save() "existed, was correct as far as anyone could
#     tell, and was called from nowhere."
#
# Both classes are statically visible. Nothing dynamic is needed to notice that
# a region has no entry in p_region_specs, that a dirty bit has no consumer, or
# that a writer does not use the stage/write/commit protocol the checksum
# depends on. So this is a source-only guard: it needs no build, no NVMe and no
# boot, which is also why it runs in CI's verify job on every push.
#
# ─── The invariants, and what breaks if one is dropped ────────────────────
#   A. Files exist (else exit 2 — the guard could not be evaluated at all).
#   B. kernel/env_ckpt.c is in the Makefile's X86_C_SRC, or it is never built.
#   C. CKPT_REGION_ENV is defined AND CKPT_NUM_REGIONS is exactly one past it.
#      Getting this wrong is silent: ckpt_mark_all_dirty() computes
#      (1u << CKPT_NUM_REGIONS) - 1, so a bit at or above the count is simply
#      never set on a full checkpoint, and environments quietly stop surviving.
#   D. The LBA block keeps persist.h's discipline: one full frame between the
#      header and the array, after the TLS anchor, ending below STREAM_DIR_LBA.
#   E. Every PERSIST_MAGIC_* value is distinct. persist_region_trusted()
#      resolves a region by MAGIC, so two regions sharing one would alias —
#      and the second would be trusted on the first's checksum.
#   F. persist_environments() writes through stage_hdr -> persist_write_array
#      -> persist_region_commit. Replacing any step with a bare nvme_write_sync
#      produces a region with no checksum, which persist_scan_regions() then
#      refuses on the next boot — presenting as corruption.
#   G. The writer is reachable from BOTH paths: checkpoint_trigger()'s dirty
#      walk and persist.c's deferred drain. The deferred path is easy to miss
#      and its absence loses every record written inside a transaction bracket.
#   H. The region's p_region_specs span is sizeof(env_ckpt_table) — the bytes
#      the writer writes. A mismatch fails verification on the next boot.
#   I. The restore block reads the header, reads the bytes into STAGING, and
#      hands them to env_ckpt_adopt(). Reading into the live table would defeat
#      the all-or-nothing property before adopt() ever runs.
#   J. checkpoint_mgr.c's dirty walk actually calls the writer.
#   K. The single-frame _Static_assert is still there — the writer writes one
#      span, so the array outgrowing a frame must be a build error.
#   L. adopt() clears the table BEFORE it validates. This is the ordering that
#      makes "refusal over partial application" true.
#   M. Every host test whose build command links kernel/persist.c also links
#      kernel/env_ckpt.c. persist.c now references env_ckpt_table[] and the
#      adopt/after_restore entry points, so a test that omits the file fails at
#      LINK time — and the repo's own process_host_stubs.h header exists
#      because exactly this class of omission ("one test gained the stubs and
#      another did not, and the suite went red at the linker") already happened
#      once.
#   N. The quiesce API is declared in env_ckpt.h AND defined in env_ckpt.c.
#      A declaration with no definition is a link error at best and, if the
#      writer quietly stopped calling it, a capture that describes a moving
#      target with nobody noticing.
#   O. persist_environments() quiesces BEFORE it stages its header and releases
#      AFTER it commits, with no `return` between the two. The ordering is the
#      feature (an un-frozen capture is not a checkpoint), and the absence of an
#      early return is what stops a failed capture from leaving a tenant frozen
#      with nothing on disk to show for it — v0.2 §4's `leak-unpause` tooth.
#   P. The two flag bits exist, are distinct and non-zero, an unknown one is
#      refused, and that refusal has a code AND a rendering. A flag the gate
#      does not know is refused rather than skipped, so a missing refusal arm
#      would turn a foreign record into a silently accepted one.
#   Q. The restore block re-applies the recorded pauses AFTER adopt(), so a
#      partition that was paused when the capture ran comes back paused.
#   R. env_service_destroy() drops the environment's record. Without it a
#      destroyed environment's record outlives it and the next restore brings
#      back an environment an operator removed.
#   S. Every host test that links kernel/env_ckpt.c but NOT kernel/partition.c
#      includes tests/partition_host_stubs.h. env_ckpt.c's quiesce calls
#      partition_pause/_resume/_is_paused/_exists, so such a test otherwise fails
#      at LINK time — the same class of omission clause M exists for.
#
# ─── Teeth ────────────────────────────────────────────────────────────────
# tests/env_ckpt_check_smoke.sh mutates a copy of the tree once per invariant
# and requires this guard to go red, then requires it green on the untouched
# tree. Every tooth names its invariant.
#
# Optional argument: the repository root to inspect (the smoke's hermetic seam;
# every real caller passes nothing and gets this script's own parent).
#
# Exit: 0 all invariants hold, 1 one failed, 2 precondition missing.
set -u

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT"

fail=0
ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }

# has <file> <fixed string> -> 0 when present
has() { grep -qF -- "$2" "$1" 2>/dev/null; }

# ── A. Preconditions ──────────────────────────────────────────────────────
for f in kernel/env_ckpt.h kernel/env_ckpt.c kernel/persist.c kernel/persist.h \
         kernel/checkpoint_delta.h kernel/checkpoint_mgr.c kernel/env_service.c Makefile \
         tests/env_ckpt_host_test.c tests/partition_host_stubs.h; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f is missing — cannot evaluate the P1a wiring at all" >&2
        exit 2
    fi
done
ok "A. every file the P1a wiring lives in is present"

# ── B. The new translation unit is actually built ─────────────────────────
if grep -qE "^[[:space:]]+kernel/env_ckpt\.c " Makefile; then
    ok "B. Makefile X86_C_SRC builds kernel/env_ckpt.c"
else
    bad "B. kernel/env_ckpt.c is not in the Makefile's X86_C_SRC — the record layer would never be linked into the kernel"
fi

# ── C. The region bit is inside the mask ──────────────────────────────────
env_bit=$(grep -E "^#define CKPT_REGION_ENV " kernel/checkpoint_delta.h | awk '{print $3}')
num_regions=$(grep -E "^#define CKPT_NUM_REGIONS " kernel/checkpoint_delta.h | awk '{print $3}')
if [ -z "${env_bit:-}" ]; then
    bad "C. CKPT_REGION_ENV is not defined — the environment checkpoint cannot be marked dirty or included in a checkpoint"
elif [ -z "${num_regions:-}" ]; then
    bad "C. CKPT_NUM_REGIONS is not defined"
elif [ "$num_regions" -ne "$((env_bit + 1))" ]; then
    bad "C. CKPT_NUM_REGIONS is $num_regions but CKPT_REGION_ENV is $env_bit — ckpt_mark_all_dirty() sets only the low $num_regions bits, so a full checkpoint would skip (or an off-by-one would overrun) the environment region"
else
    ok "C. CKPT_REGION_ENV=$env_bit is the last bit under CKPT_NUM_REGIONS=$num_regions, so a full checkpoint includes it"
fi

# ── D. LBA discipline ─────────────────────────────────────────────────────
hdr=$(grep -E "^#define PERSIST_ENV_CKPT_HDR_LBA " kernel/persist.h | awk '{print $3}' | tr -d 'ULL')
ent=$(grep -E "^#define PERSIST_ENV_CKPT_ENT_LBA " kernel/persist.h | awk '{print $3}' | tr -d 'ULL')
tls=$(grep -E "^#define PERSIST_TLS_LBA " kernel/persist.h | awk '{print $3}' | tr -d 'ULL')
sd=$(grep -E "^#define STREAM_DIR_LBA " kernel/stream.h | awk '{print $3}' | tr -d 'ULL')
if [ -z "${hdr:-}" ] || [ -z "${ent:-}" ]; then
    bad "D. PERSIST_ENV_CKPT_HDR_LBA / PERSIST_ENV_CKPT_ENT_LBA are not both defined"
elif [ "$((ent - hdr))" -lt 8 ]; then
    bad "D. the entry array (LBA $ent) starts less than one frame after the header (LBA $hdr) — they overlap"
elif [ -n "${tls:-}" ] && [ "$hdr" -le "$tls" ]; then
    bad "D. the environment region (LBA $hdr) does not sit after the TLS anchor (LBA $tls); it would share a frame with it"
elif [ -n "${sd:-}" ] && [ "$((ent + 8))" -gt "$sd" ]; then
    bad "D. the environment entry frame ends at $((ent + 8)), past STREAM_DIR_LBA ($sd) — it would land in the stream directory"
else
    ok "D. the environment region sits at LBA $hdr/$ent: one full frame apart, after the TLS anchor ($tls), ending $((ent + 8)) < STREAM_DIR_LBA ($sd)"
fi
if has kernel/persist.h "void persist_environments(void);"; then
    ok "D. persist.h declares persist_environments()"
else
    bad "D. persist.h does not declare persist_environments()"
fi

# ── E. Magics are unique (persist_region_trusted() resolves by magic) ─────
dupes=$(grep -oE "0xCAFE[0-9A-Fa-f]{12}" kernel/persist.h | sort | uniq -d)
if [ -n "$dupes" ]; then
    bad "E. these PERSIST_MAGIC_* values are used by more than one region: $dupes — persist_region_trusted() matches on magic, so the regions would alias and one would be trusted on the other's checksum"
else
    ok "E. every PERSIST_MAGIC_* value is distinct, so no two regions alias"
fi

# ── F. The writer speaks the persistence protocol ─────────────────────────
writer=$(awk '/^void persist_environments\(void\) \{/,/^\}/' kernel/persist.c)
if [ -z "$writer" ]; then
    bad "F. persist_environments() is not defined in kernel/persist.c"
else
    missing=""
    case "$writer" in *"ckpt_mark_dirty(CKPT_REGION_ENV)"*) ;; *) missing="$missing mark-dirty";; esac
    case "$writer" in *"persist_defer_note(PERSIST_PEND_ENV)"*) ;; *) missing="$missing defer-note";; esac
    case "$writer" in *"stage_hdr(PERSIST_ENV_CKPT_HDR_LBA, PERSIST_MAGIC_ENV_CKPT"*) ;; *) missing="$missing stage_hdr";; esac
    case "$writer" in *"persist_write_array(env_ckpt_table,"*) ;; *) missing="$missing write_array(env_ckpt_table)";; esac
    case "$writer" in *"PERSIST_ENV_CKPT_ENT_LBA"*) ;; *) missing="$missing entry-lba";; esac
    case "$writer" in *"persist_region_commit()"*) ;; *) missing="$missing commit";; esac
    if [ -n "$missing" ]; then
        bad "F. persist_environments() is missing the protocol steps:$missing — without stage_hdr/persist_write_array/persist_region_commit the region is written without a checksum, and persist_scan_regions() refuses it on the next boot"
    else
        ok "F. persist_environments() marks the region dirty, stages its header, writes env_ckpt_table[], and commits with the barrier+checksum protocol"
    fi
fi

# ── G. Reachable from both write paths ────────────────────────────────────
if ! grep -qE "^#define PERSIST_PEND_ENV " kernel/persist.c; then
    bad "G. PERSIST_PEND_ENV has no pending bit, so the deferred drain cannot flush the region"
elif grep -qE "if \(pend & PERSIST_PEND_ENV\)[[:space:]]+persist_environments\(\);" kernel/persist.c; then
    ok "G. persist.c's deferred drain flushes the region (records written inside a transaction bracket are not lost)"
else
    bad "G. persist.c's deferred drain does not call persist_environments() — every environment record written inside a persist_defer_begin/end bracket would be silently dropped"
fi

# ── H. The checksum span matches the bytes the writer writes ──────────────
spec=$(awk "/^static const struct PersistRegionSpec p_region_specs/,/^};/" kernel/persist.c \
       | grep -A2 "PERSIST_ENV_CKPT_HDR_LBA")
if [ -z "$spec" ]; then
    bad "H. the region has no entry in p_region_specs — persist_scan_regions() would never checksum it, so a torn write would be loaded as if it were complete"
elif printf '%s' "$spec" | grep -q "sizeof(env_ckpt_table)"; then
    ok "H. p_region_specs covers the region with a span of sizeof(env_ckpt_table) — exactly the bytes persist_environments() writes"
else
    bad "H. p_region_specs' span for the region is not sizeof(env_ckpt_table); if it disagrees with what the writer writes, every boot fails the region's own checksum and reports corruption"
fi

# ── I. The restore block reads into staging and refuses via adopt() ───────
restore=$(awk '/── 17\. Environment checkpoint records/,/^    }$/' kernel/persist.c)
if [ -z "$restore" ]; then
    bad "I. persist_restore_all() has no environment-record block — a snapshot would be written every boot and never read back"
else
    missing=""
    case "$restore" in *"nvme_read_sync(PERSIST_ENV_CKPT_HDR_LBA"*) ;; *) missing="$missing header-read";; esac
    case "$restore" in *"persist_read_array(p_env_staging,"*) ;; *) missing="$missing staging-read";; esac
    case "$restore" in *"env_ckpt_adopt(p_env_staging,"*) ;; *) missing="$missing adopt";; esac
    case "$restore" in *"env_ckpt_after_restore()"*) ;; *) missing="$missing after-restore";; esac
    if [ -n "$missing" ]; then
        bad "I. the restore block is incomplete:$missing — reading straight into env_ckpt_table[] would apply a torn or foreign snapshot before anything had a chance to refuse it, which is the property P1a is built around"
    elif grep -qF "persist_read_array(env_ckpt_table" kernel/persist.c; then
        bad "I. persist_restore_all() reads the snapshot straight into the LIVE table; it must read into p_env_staging and let env_ckpt_adopt() validate it first"
    else
        ok "I. the restore block reads the header and the bytes into staging, then hands them to env_ckpt_adopt()"
    fi
fi

# ── J. A checkpoint actually writes it ────────────────────────────────────
if grep -qE "if \(dirty & \(1u << CKPT_REGION_ENV\)\)[[:space:]]+persist_environments\(\);" kernel/checkpoint_mgr.c; then
    ok "J. checkpoint_trigger()'s dirty-region walk calls persist_environments()"
else
    bad "J. checkpoint_trigger() does not call persist_environments() for CKPT_REGION_ENV — a checkpoint would report success while writing no environment records"
fi

# ── K. The frame-fit invariant is a build error, not a hope ───────────────
if grep -qF "_Static_assert(sizeof(struct EnvCkptRecord) * ENV_CKPT_MAX <= 4096" kernel/env_ckpt.c; then
    ok "K. env_ckpt.c still asserts the whole record array fits one NVMe frame"
else
    bad "K. the single-frame _Static_assert is gone — persist_environments() writes the array as ONE span, so letting it outgrow 4 KiB would truncate it silently"
fi

# ── L. adopt() clears before it validates ─────────────────────────────────
if ! grep -qE "^#define ENV_CKPT_MAX[[:space:]]" kernel/env_ckpt.h; then
    bad "L. ENV_CKPT_MAX is not defined in env_ckpt.h"
else
    clear_line=$(grep -nF "ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));" kernel/env_ckpt.c | head -3 | cut -d: -f1 | tr '\n' ' ')
    valid_line=$(grep -nF "env_ckpt_valid(&staging[i])" kernel/env_ckpt.c | head -1 | cut -d: -f1)
    first_clear=$(printf '%s' "$clear_line" | awk '{print $1}')
    if [ -z "${valid_line:-}" ] || [ -z "${first_clear:-}" ]; then
        bad "L. could not locate adopt()'s pre-clear and its per-record validation — the guard's own anchors have moved"
    elif [ "$first_clear" -gt "$valid_line" ]; then
        bad "L. adopt() validates records before it clears the table — a snapshot that fails validation at record k would leave records 0..k-1 applied, which is precisely the half-restored environment P1a refuses"
    elif ! grep -qF "ec_memset(env_ckpt_table, 0, (uint32_t)sizeof(env_ckpt_table));" kernel/env_ckpt.c; then
        bad "L. adopt() does not clear the table at all on the refusal paths"
    else
        ok "L. adopt() empties the table before it validates, so every refusal leaves no environment behind ($(printf '%s' "$clear_line" | wc -w) clear sites)"
    fi
fi

# ── M. Every host test that links persist.c links env_ckpt.c too ──────────
# Extracts each test's own "Build and run:" gcc command exactly the way
# tests/run_all.sh does (the header comment IS the build recipe), so this
# guard and the runner cannot disagree about what a test links.
missing_link=""
for t in tests/*_host_test.c; do
    cmd=$(awk '
        /[*] *gcc / { grab=1 }
        grab {
            line = $0
            gsub(/\r/, "", line)
            sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
            print line
            if (line !~ /[\\][[:space:]]*$/) { exit }
        }
    ' "$t")
    case "$cmd" in
        *"kernel/persist.c"*)
            case "$cmd" in
                *"kernel/env_ckpt.c"*) ;;
                *) missing_link="$missing_link $(basename "$t")" ;;
            esac ;;
    esac
done
if [ -n "$missing_link" ]; then
    bad "M. these host tests link kernel/persist.c but not kernel/env_ckpt.c, so they fail at LINK time on env_ckpt_table[]/env_ckpt_adopt():$missing_link"
else
    ok "M. every host test that links kernel/persist.c also links kernel/env_ckpt.c"
fi

# ── N. The quiesce API is declared and defined ─────────────────────────────
missing=""
for sym in env_ckpt_quiesce_for_capture env_ckpt_release_capture \
           env_ckpt_apply_restored_pauses env_ckpt_drop_env; do
    has kernel/env_ckpt.h "$sym("   || missing="$missing header:$sym"
    has kernel/env_ckpt.c "$sym("   || missing="$missing impl:$sym"
done
if ! has kernel/env_ckpt.c '#include "partition.h"'; then
    missing="$missing include:partition.h"
fi
if [ -n "$missing" ]; then
    bad "N. the quiesce API is incomplete:$missing — a capture that cannot freeze what it captures describes a moving target, and a declared-but-undefined entry point is a link error"
else
    ok "N. env_ckpt.h declares and env_ckpt.c defines the quiesce/release/restore-pause/drop entry points, over partition.c's primitives"
fi

# ── O. The capture quiesces before it writes, and always releases ─────────
wbody=$(awk '/^void persist_environments\(void\) \{/,/^\}/' kernel/persist.c)
q_line=$(printf '%s\n' "$wbody" | grep -nF "env_ckpt_quiesce_for_capture(&q);" | head -1 | cut -d: -f1)
s_line=$(printf '%s\n' "$wbody" | grep -nF "stage_hdr(PERSIST_ENV_CKPT_HDR_LBA" | head -1 | cut -d: -f1)
w_line=$(printf '%s\n' "$wbody" | grep -nF "persist_write_array(env_ckpt_table," | head -1 | cut -d: -f1)
c_line=$(printf '%s\n' "$wbody" | grep -nF "persist_region_commit();" | head -1 | cut -d: -f1)
r_line=$(printf '%s\n' "$wbody" | grep -nF "env_ckpt_release_capture(&q);" | head -1 | cut -d: -f1)
if [ -z "${q_line:-}" ] || [ -z "${r_line:-}" ]; then
    bad "O. persist_environments() does not quiesce (or does not release): an un-frozen capture is not a checkpoint, and an un-released one leaves a tenant paused forever"
elif [ -z "${s_line:-}" ] || [ -z "${w_line:-}" ] || [ -z "${c_line:-}" ]; then
    bad "O. could not locate the writer's stage/write/commit anchors — the guard's own anchors have moved"
elif [ "$q_line" -ge "$s_line" ]; then
    bad "O. the capture stages its header (line $s_line) before it quiesces (line $q_line) — what is written down is then a moving target"
elif [ "$r_line" -le "$c_line" ]; then
    bad "O. the capture releases (line $r_line) before it commits (line $c_line) — the tenant would be running again while the snapshot is still being written"
else
    returns_after=$(printf '%s\n' "$wbody" | tail -n +"$q_line" | grep -cE "^[[:space:]]*return")
    if [ "$returns_after" -ne 0 ]; then
        bad "O. $returns_after return(s) sit between the quiesce (line $q_line) and the end of persist_environments() — a capture that bails out there leaves a tenant frozen with nothing on disk to show for it (v0.2 §4's `leak-unpause` tooth)"
    else
        ok "O. persist_environments() quiesces (line $q_line) before staging, writes, commits, then releases (line $r_line) on a single straight-line exit"
    fi
fi

# ── P. The flag bits exist, are distinct, and an unknown one is refused ───
qu=$(grep -E "^#define ENV_CKPT_FLAG_QUIESCED " kernel/env_ckpt.h | awk '{print $3}' | tr -d 'uULL')
pp=$(grep -E "^#define ENV_CKPT_FLAG_PARTITION_PAUSED " kernel/env_ckpt.h | awk '{print $3}' | tr -d 'uULL')
if [ -z "${qu:-}" ] || [ -z "${pp:-}" ]; then
    bad "P. ENV_CKPT_FLAG_QUIESCED and ENV_CKPT_FLAG_PARTITION_PAUSED must both be defined — QUIESCED without the writer is the vacuity this phase removes, and PARTITION_PAUSED is what a restore replays"
elif [ "$((qu))" -eq 0 ] || [ "$((pp))" -eq 0 ] || [ "$((qu))" -eq "$((pp))" ]; then
    bad "P. the two flag bits must be non-zero and distinct (got $qu and $pp) — sharing a bit makes \"frozen for this capture\" indistinguishable from \"the operator paused it\""
elif ! has kernel/env_ckpt.h "#define ENV_CKPT_REFUSE_BAD_FLAGS"; then
    bad "P. ENV_CKPT_REFUSE_BAD_FLAGS is not defined, so env_ckpt_valid()'s unknown-flag refusal has no code"
elif ! has kernel/env_ckpt.c "rec->flags & ~(ENV_CKPT_FLAG_QUIESCED | ENV_CKPT_FLAG_PARTITION_PAUSED)"; then
    bad "P. env_ckpt_valid() no longer refuses an unknown flag bit — a record whose meaning is partly unknown would be accepted"
elif ! has kernel/env_ckpt.c "case ENV_CKPT_REFUSE_BAD_FLAGS:"; then
    bad "P. the BAD_FLAGS refusal has no rendering — the serial transcript would say nothing about why the record was refused"
else
    ok "P. QUIESCED=$qu and PARTITION_PAUSED=$pp are distinct, and an unknown flag bit is refused with its own code and rendering"
fi

# ── Q. The restore re-applies the recorded pauses, after adopt() ───────────
if [ -z "${restore:-}" ]; then
    bad "Q. no restore block to check (see I)"
else
    ar_line=$(printf '%s\n' "$restore" | grep -nF "env_ckpt_after_restore()" | head -1 | cut -d: -f1)
    ap_line=$(printf '%s\n' "$restore" | grep -nF "env_ckpt_apply_restored_pauses()" | head -1 | cut -d: -f1)
    if [ -z "${ap_line:-}" ]; then
        bad "Q. the restore never calls env_ckpt_apply_restored_pauses() — a partition that was paused when the capture ran would come back RUNNING"
    elif [ -z "${ar_line:-}" ] || [ "$ap_line" -le "$ar_line" ]; then
        bad "Q. the restore re-applies pauses before after_restore() has settled the live set, so it would act on records that did not survive compaction"
    else
        ok "Q. the restore re-applies the snapshot's pauses (line $ap_line) after after_restore() (line $ar_line), so a paused partition is restored paused"
    fi
fi

# ── R. A destroy drops the environment's record ────────────────────────────
destroy=$(awk '/^int env_service_destroy\(/,/^\}/' kernel/env_service.c)
if [ -z "$destroy" ]; then
    bad "R. env_service_destroy() is not defined in kernel/env_service.c"
else
    case "$destroy" in
        *"env_ckpt_drop_env(partition, env_id)"*) ok "R. env_service_destroy() drops the checkpoint record with the environment, so a removed environment is not restored" ;;
        *) bad "R. env_service_destroy() does not call env_ckpt_drop_env() — a destroyed environment's record outlives it and the next restore brings back an environment an operator removed" ;;
    esac
fi

# ── S. env_ckpt.c's partition dependency is stubbed where partition.c is not
missing_stub=""
for t in tests/*_host_test.c; do
    cmd=$(awk '
        /[*] *gcc / { grab=1 }
        grab {
            line = $0
            gsub(/\r/, "", line)
            sub(/^[[:space:]]*\*[[:space:]]?/, "", line)
            print line
            if (line !~ /[\\][[:space:]]*$/) { exit }
        }
    ' "$t")
    case "$cmd" in
        *"kernel/env_ckpt.c"*)
            case "$cmd" in
                *"kernel/partition.c"*) ;;
                *) grep -qF "tests/partition_host_stubs.h" "$t" || \
                       missing_stub="$missing_stub $(basename "$t")" ;;
            esac ;;
    esac
done
if [ -n "$missing_stub" ]; then
    bad "S. these host tests link kernel/env_ckpt.c without kernel/partition.c and do not include tests/partition_host_stubs.h, so they fail at LINK time on partition_pause/_resume/_is_paused/_exists:$missing_stub"
else
    ok "S. every host test that links kernel/env_ckpt.c without kernel/partition.c includes the quiesce stubs"
fi

if [ "$fail" -ne 0 ]; then
    echo
    echo "env_ckpt_check: the P1a environment checkpoint is not wired the way it is written."
    exit 1
fi
echo
echo "env_ckpt_check: P1a environment checkpoint wiring is intact."
exit 0
