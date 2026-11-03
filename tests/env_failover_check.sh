#!/usr/bin/env bash
# tests/env_failover_check.sh — guards the WIRING of POSIX-Environments v0.2
# Phase P3's failover half (destination (b), §11 Q5), not its logic.
#
# ─── What this guard is for ──────────────────────────────────────────────────
# P3 increments 1–8 all concern MIGRATION: an operator asks for a partition
# to move, and the environment moves with it. The DEATH path is separate and
# was untouched by every one of them. Before this increment,
# failover_recover_from() adopted a dead node's partitions by ownership flip
# and said nothing at all about the environments those partitions held —
# Appendix B's own named gap ("failover_recover_from() adopts ownership
# only — a dead node's environments vanish with no name in the report").
#
# §11 Q5 asked which of two destinations failover should take, and the answer
# chosen is (b): adopt the PARTITION, restore from the survivor's own durable
# storage what that storage still holds, and REPORT THE REST BY NAME. (a) —
# bulk-transferring a surviving environment to the survivor — is the
# PASE-grade answer and a much larger lift; the leader's checkpoint
# broadcast is 24 KiB and one environment's regions are 5.5 MiB, so (a) is
# not a small extension of what exists. (b) is honest about what failover can
# actually restore today, and "today" moved twice: increment 9 landed the
# REPORT (every environment of an adopted partition named, whether or not
# anything could be done about it), and increment 11 landed the RESTORE
# HALF — the survivor replays each adopted partition's durable record
# through the same driver a reboot uses, and the report became a per-
# environment OUTCOME: restored, or not restored with its reason.
#
# That makes both halves deliverables worth guarding, which is why a source
# guard is the right instrument: the report is loops that print names, and
# a loop that prints names can be deleted, unscoped, left reading a stale
# counter, or made to call a replay that never runs — each independently,
# each silently, and none of them visible in a run where no node happened to
# die with an environment.
#
# ─── The invariants, and what breaks if one is dropped ────────────────────
#   A. Files exist (else exit 2 — the wiring could not be evaluated at all).
#   B. The state tree carries the NAMES: state_tree.h defines
#      ST_NODE_ENVIRONMENT and state_tree.c emits a node per live record.
#      Without this the survivor's checkpoint simply does not mention the
#      environments, and no downstream report could name them.
#   C. The names are emitted AFTER the partition nodes, so the parent lookup
#      can attach each environment to its partition. An environment node
#      parented to root is a name with no partition behind it.
#   D. failover.c WALKS those nodes and reports each by name as NOT
#      restored. This is the deliverable itself.
#   E. The report is SCOPED to partitions this node just adopted — the
#      owner check. An environment in a partition this node does not own
#      belongs to whoever owns that partition now; naming it here would
#      point an operator at state that is not this node's to restore, which
#      is the same defect as silence with better manners.
#   F. The counter is reset at FUNCTION ENTRY, before any early return. A
#      reset inside the reporting block leaves a recovery that adopts
#      nothing reporting the PREVIOUS recovery's count — a stale number
#      that reads as a real gap.
#   G. The accessor is declared in failover.h, defined in failover.c, and
#      PINNED by a host test that links the real failover.c and asserts both
#      the count and the zero. A report only reachable through a log grep is
#      a claim; a count a test can assert is a property.
#   H. ST_VERSION was bumped with the node type, so a v1 tree is REFUSED by
#      name rather than read by a parser that silently drops the environment
#      names — which is exactly the silent vanish (b) exists to prevent.
#   I. The restore half exists: each named environment's durable record is
#      armed for replay (env_ckpt_restore_arm, the per-record form — the
#      boot-shaped restore_begin() would re-arm the survivor's OWN live
#      environments). Without it the report is back to naming a gap that is
#      now optional.
#   J. The armed records are drained through the STANDARD driver
#      (env_service_restore_pending()) — the create round trip, identity
#      check, pause ledger and payload pour are all its, and a second copy
#      of those is a second place for them to rot. Arming without driving
#      leaves every record pending forever.
#   K. A record the replay REFUSED is never counted as restored. Both
#      outcomes clear the pending bit, so the per-slot refused mark is the
#      only thing that separates them; losing this clause makes a refusal
#      read as a success — the silent lie, told about a dead node's data.
#   L. The restored counter is declared in failover.h, defined in failover.c,
#      reset at FUNCTION ENTRY beside its unrestored sibling, and pinned by
#      the host test on both the count and the zero.
#   M. The refused mark is kept in step with the table: refuse() sets the
#      slot, drop() SHIFTS it with the record (a bit left behind by a drop
#      points at a different environment), restore_begin() clears it.
#
# ─── Teeth ──────────────────────────────────────────────────────────────────
# Proven in tests/env_failover_check_smoke.sh. The worth-stating ones are
# `silent-failover` — the adoption completes and says nothing about the
# environments (increment 9's defect verbatim) — and `refused-as-restored`
# — the replay refuses a record and the report calls it restored
# (increment 11's, the lie only this pairing can tell). Expected: red on D
# and K respectively.
#
# ─── Scope note ─────────────────────────────────────────────────────────────
# This guard is SOURCE-ONLY: it reads the tree and needs no build, no NVMe
# and no cluster, so it runs in CI's verify job on every push. The runtime
# behaviour is pinned by tests/checkpoint_recovery_host_test.c (Scenarios
# 4b and 4c), which drives a real failover_recover_from() over a tree that
# carries environment names and a record store the driver replays from. The
# roadmap's separate tests/failover_adoption_live_check.sh runtime guard —
# boot a cluster, kill a node, watch a survivor adopt — is the live arm this
# file deliberately does NOT try to be; see the note in its own header
# about GUARD-KIND.
#
# GUARD-KIND: source
#
# Usage: tests/env_failover_check.sh [root]
#   root defaults to the repository containing this script.

ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$ROOT" || exit 2

fail=0
ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }

# has <file> <fixed string> -> 0 when present
has() { grep -qF -- "$2" "$1" 2>/dev/null; }

# NOTE on every clause below that greps kernel/*.c: match the CODE, never a
# bare identifier. This repository has paid for that lesson once already —
# env_migration_check.sh's original tooth S matched the function NAME in
# net/dspp.c, and dspp.c's own comments mention that name, so the guard
# reddened on nothing. Every identifier asserted against a .c file here is
# anchored to its assignment, its call with arguments, or its emission — so a
# comment mentioning it cannot hold a clause up.

# ── A. Preconditions ──────────────────────────────────────────────────────
for f in kernel/state_tree.h kernel/state_tree.c \
         kernel/failover.c kernel/failover.h \
         kernel/env_ckpt.c kernel/env_ckpt.h kernel/env_service.h \
         tests/checkpoint_recovery_host_test.c \
         tests/env_failover_check.sh; do
    if [ ! -f "$f" ]; then
        echo "ABORT: $f is missing — cannot evaluate the P3 failover (b) wiring at all" >&2
        exit 2
    fi
done
ok "A. every file the failover-by-name wiring lives in is present"

# ── B. The tree carries the environment NAMES ─────────────────────────────
# Each sub-check names the CODE, not the bare identifier: ST_NODE_ENVIRONMENT
# appears in state_tree.c's own comments and in the serializer's `case`, so a
# bare-identifier grep would hold this clause up after the emission itself was
# deleted — the exact "comments are not wiring" defect this repo's guard
# history records.
st_emit=$(awk '/nodes\[n\]\.type         = ST_NODE_ENVIRONMENT/{print NR; exit}' kernel/state_tree.c)
if ! has kernel/state_tree.h "#define ST_NODE_ENVIRONMENT 5"; then
    bad "B. state_tree.h no longer defines ST_NODE_ENVIRONMENT — the checkpoint cannot mention a dead node's environments at all, so nothing downstream can name them"
elif [ -z "$st_emit" ]; then
    bad "B. state_tree.c no longer EMITS an environment node — the type exists and nothing produces it, which is the wiring-removed failure this guard exists for"
elif ! has kernel/state_tree.c "&env_ckpt_table[i]"; then
    # A plain has, anchored to the ADDRESS-OF expression: the read is code,
    # not a mention, and anchoring to `&env_ckpt_table[i]` means a comment
    # that merely names the table cannot hold the clause up. (An earlier
    # draft used awk to require the read to follow the emission line — it
    # reddened on the REAL tree, because the read sits at the top of the
    # loop and the emission at the bottom. The vacuity control caught it.)
    bad "B. state_tree.c no longer reads env_ckpt_table to build the environment node — the names have no source"
else
    ok "B. the state tree emits one ST_NODE_ENVIRONMENT node per live env_ckpt_table record"
fi

# ── C. The names are emitted AFTER the partition nodes ────────────────────
# The parent lookup scans nodes emitted SO FAR for the partition node, so an
# environment emitted before its partition can never find it and lands
# parented to root — a name with no partition behind it.
st_part=$(awk '/type         = ST_NODE_PARTITION/{print NR; exit}' kernel/state_tree.c)
st_env=$(awk '/type         = ST_NODE_ENVIRONMENT/{print NR; exit}' kernel/state_tree.c)
if [ -z "$st_part" ] || [ -z "$st_env" ]; then
    bad "C. could not locate the partition or environment emission in state_tree.c — the ordering claim cannot be evaluated"
elif [ "$st_part" -ge "$st_env" ]; then
    bad "C. the environment nodes are emitted BEFORE the partition nodes — each environment's parent lookup would miss its partition and attach it to root, so the tree carries a name with no partition behind it"
else
    ok "C. environment nodes are emitted after the partition nodes, so each can find its parent"
fi

# ── D. failover WALKS the names and reports them ──────────────────────────
fo_walk=$(awk '/^int failover_recover_from/{f=1} f && /nodes\[i\].type != ST_NODE_ENVIRONMENT/{print NR; exit}' kernel/failover.c)
if [ -z "$fo_walk" ]; then
    bad "D. failover_recover_from() no longer walks the environment nodes — a dead node's environments vanish with no name in the report, which is Appendix B's named gap come straight back"
elif ! has kernel/failover.c "was NOT restored"; then
    bad "D. failover.c no longer reports an unrestored environment BY NAME — the adoption is silent about them again"
else
    ok "D. failover_recover_from() walks the environment nodes and names each as not restored"
fi

# ── E. The report is scoped to the partitions this node adopted ───────────
fo_scope=$(awk '/nodes\[i\].type != ST_NODE_ENVIRONMENT/{f=1} f && /partition_get_owner_node/{print NR; exit}' kernel/failover.c)
if [ -z "$fo_scope" ]; then
    bad "E. the environment report is no longer scoped by ownership — it would name environments in partitions this node does not own, pointing an operator at state that is not theirs to restore (the same defect as silence, with better manners)"
else
    ok "E. the report names only environments of the partitions this node actually adopted"
fi

# ── F. The counter resets at function entry, before any early return ──────
# The lookup is anchored to an INDENTED line on purpose: the file-scope
# `static uint32_t last_unrestored_envs = 0;` is a DECLARATION (statics
# zero-init anyway), not a reset, and an unanchored search let that one
# satisfy this clause after the real reset was deleted. A vacuous clause and
# a vacuous tooth are the same failure seen from two sides — the smoke's
# `stale-count` tooth is what exposed it.
fo_entry=$(awk '/^int failover_recover_from/{print NR; exit}' kernel/failover.c)
fo_reset=$(awk '/^    last_unrestored_envs = 0;/{print NR; exit}' kernel/failover.c)
fo_walk_line=$(awk '/nodes\[i\].type != ST_NODE_ENVIRONMENT/{print NR; exit}' kernel/failover.c)
if [ -z "$fo_reset" ]; then
    bad "F. failover_recover_from() no longer resets the unrestored-environment count — a recovery that adopts nothing reports the previous recovery's gap as its own"
elif [ -z "$fo_entry" ] || [ -z "$fo_walk_line" ] || [ "$fo_reset" -ge "$fo_walk_line" ]; then
    bad "F. the count is reset inside or after the reporting walk rather than at function entry — every early return (no checkpoint, wrong source, no partitions) would then leave the PREVIOUS recovery's count standing, and a caller would read a stale number as this recovery's real gap"
else
    ok "F. the count is reset at function entry, so an early return reports zero rather than a stale gap"
fi

# ── G. The accessor is declared, defined, and pinned by a host test ───────
if ! has kernel/failover.h "uint32_t failover_last_unrestored_envs(void);"; then
    bad "G. failover.h no longer declares failover_last_unrestored_envs() — the count has no contract, so nothing outside failover.c can read the gap"
elif ! has kernel/failover.c "uint32_t failover_last_unrestored_envs(void)"; then
    bad "G. failover.c no longer defines failover_last_unrestored_envs() — declared but not implemented"
elif ! has tests/checkpoint_recovery_host_test.c "failover_last_unrestored_envs() == 2"; then
    bad "G. the host test no longer pins the unrestored-environment COUNT — a report only reachable by grepping a serial line is a claim, not a property"
elif ! has tests/checkpoint_recovery_host_test.c "failover_last_unrestored_envs() == 0"; then
    bad "G. the host test no longer pins the ZERO after a recovery that adopts nothing — without it a stale count passes every other assertion in this clause"
else
    ok "G. the count is declared, defined, and pinned on both sides by a host test that links the real failover.c"
fi

# ── H. The version moved with the node type ───────────────────────────────
if ! has kernel/state_tree.h "#define ST_VERSION  2"; then
    bad "H. state_tree.h no longer declares ST_VERSION 2 — a tree carrying environment nodes would be version 1 to a parser that silently drops them, which is the silent vanish this whole increment exists to prevent"
elif ! has kernel/state_tree.h "num_environments"; then
    bad "H. the serialized header no longer counts environments — v2's node type arrived without the field that says it is in use"
else
    ok "H. the serialized format is version 2 with its environment count, so a v1 tree is refused by name rather than misread"
fi

# ── I. The restore half: each named record is armed for replay ────────────
# Increment 9 named the gap; increment 11 closes what it can. The anchor is
# the CALL with its arguments, so a comment mentioning the function cannot
# hold this up (the "comments are not wiring" lesson, paid for twice).
if ! has kernel/failover.c "env_ckpt_restore_arm(part_id, env_idx)"; then
    bad "I. failover.c no longer arms the adopted partitions' durable records for replay — the restore half of destination (b) is gone, and the report is back to naming a gap that is now optional"
else
    ok "I. each named environment's durable record is armed for replay (the per-record env_ckpt_restore_arm, not the boot's blanket begin)"
fi

# ── J. The armed records are drained through the standard driver ──────────
if ! has kernel/failover.c "env_service_restore_pending(&rep)"; then
    bad "J. failover.c no longer drives the replay through env_service_restore_pending() — armed records would sit pending forever and the create round trip, identity check and payload pour would each need a second, rotting copy here"
else
    ok "J. the replay is driven by env_service_restore_pending(), the same driver a reboot uses"
fi

# ── K. A refused record is never counted as restored ──────────────────────
# The both-outcomes-clear-pending fact is why this clause exists: without
# the per-slot refused mark the report reads a REFUSED record as restored.
# The sharpest tooth in the smoke (`refused-as-restored`) deletes exactly
# this query and requires the guard to name THIS clause.
if ! has kernel/failover.c "env_ckpt_restore_refused_record(p, idx)"; then
    bad "K. failover.c no longer consults the per-slot refused mark — a record the replay REFUSED clears the pending bit like a settled one, so the report would count a refusal as a restore: the silent lie this clause exists to prevent"
else
    ok "K. the outcome classifies refused records as unrestored, never restored (env_ckpt_restore_refused_record consulted per record)"
fi

# ── L. The restored counter: declared, defined, reset at entry, pinned ────
fo_rst_reset=$(awk '/^    last_restored_envs = 0;/{print NR; exit}' kernel/failover.c)
fo_walk_line=$(awk '/nodes\[i\].type != ST_NODE_ENVIRONMENT/{print NR; exit}' kernel/failover.c)
if ! has kernel/failover.h "uint32_t failover_last_restored_envs(void);"; then
    bad "L. failover.h no longer declares failover_last_restored_envs() — the restore has no countable contract, so nothing outside failover.c can read how much came back"
elif ! has kernel/failover.c "uint32_t failover_last_restored_envs(void)"; then
    bad "L. failover.c no longer defines failover_last_restored_envs() — declared but not implemented"
elif [ -z "$fo_rst_reset" ] || [ -z "$fo_walk_line" ] || [ "$fo_rst_reset" -ge "$fo_walk_line" ]; then
    bad "L. the restored count is not reset at function entry, before the walk — every early return (no checkpoint, wrong source, no partitions) would leave the PREVIOUS recovery's restored count standing, and a caller would read a stale success as this recovery's"
elif ! has tests/checkpoint_recovery_host_test.c "failover_last_restored_envs() == 2"; then
    bad "L. the host test no longer pins the restored COUNT — a serial line is a claim; a count a test asserts is a property"
elif ! has tests/checkpoint_recovery_host_test.c "failover_last_restored_envs() == 0"; then
    bad "L. the host test no longer pins the ZERO after a recovery that adopts nothing — without it a stale restored count passes every other assertion"
else
    ok "L. the restored count is declared, defined, reset at entry, and pinned on both sides by a host test"
fi

# ── M. The refused mark is kept in step with the table ────────────────────
# Three lines, three independent ways to corrupt the classification: the
# mark never set (refusals vanish), never shifted (a drop leaves it pointing
# at a DIFFERENT environment, so a healthy one is reported refused), never
# cleared (a previous pass's refusal outlives its record).
if ! has kernel/env_ckpt.c "ec_restore_refused_slot[slot] = 1;"; then
    bad "M. env_ckpt.c's restore_refuse() no longer marks the slot refused — the refused mark is never set, so every refusal classifies as a restore"
elif ! has kernel/env_ckpt.c "ec_restore_refused_slot[i] = ec_restore_refused_slot[i + 1];"; then
    bad "M. env_ckpt.c's drop() no longer shifts the refused mark with the record — after a drop the mark points at a different environment, and a healthy one is reported refused"
elif ! has kernel/env_ckpt.c "ec_memset(ec_restore_refused_slot, 0,"; then
    bad "M. env_ckpt.c's restore_begin() no longer clears the refused marks — a previous pass's refusal outlives its record and stains the next replay"
else
    ok "M. the refused mark is set on refuse, shifted on drop, cleared on begin — always describing the record it sits on"
fi

# ── N. The replica fills (a): consulted, promoted, replayed, counted ──────
# P3 destination (a) increment 3. Five links, each asserted as the CALL or
# the COMPARISON (the "comments are not wiring" rule, paid for twice):
# the lookup that finds the dead node's copy, the newer-wins ordering that
# decides between the two sources, the promotion that stages it, and the
# migration restore that replays it. The last two teeth pin the HOST TEST's
# classification arms, because this clause's whole subject is counting:
# a promotion that only ever succeeds would let a broken fallback (the
# floor under (a)) classify as a silent gap, and an unapplied stale copy
# would be invisible rather than named.
if ! has kernel/failover.c "env_replica_lookup(part_id, env_idx, &rep)"; then
    bad "N. failover.c no longer consults this survivor's replica band — a hole a replica could fill is named 'no durable record' again while destination (a)'s bytes sit unread on this node's own disk"
elif ! has kernel/failover.c "(uint64_t)rec->sequence < rep.seq"; then
    bad "N. failover.c no longer orders the two sources by capture sequence — with both a record and a replica present the newer-wins rule (the receiver's own) would have no caller, and the older copy could be applied over the newer"
elif ! has kernel/failover.c "env_replica_promote(part_id, env_idx)"; then
    bad "N. failover.c no longer promotes a found replica — the lookup would find the copy and leave it in the band, which is (a) without its third increment"
elif ! has kernel/failover.c "env_migrate_restore(part_id, env_idx)"; then
    bad "N. failover.c no longer replays the promoted bytes through env_migrate_restore() — a staged replica with no replay is a classification that never runs, and the pour would need a second copy here"
elif ! has tests/checkpoint_recovery_host_test.c "both hole environments came back from the REPLICA"; then
    bad "N. the host test no longer pins the hole-to-replica restore — the count a serial line reports is a claim, not a property"
elif ! has tests/checkpoint_recovery_host_test.c "the staged slot was dropped on the way"; then
    bad "N. the host test no longer pins the FALLBACK (a newer replica whose replay refuses falls back to (b), staged slot dropped) — without it a broken floor under (a) reads as a plain gap"
elif ! has tests/checkpoint_recovery_host_test.c "a replica OLDER than the durable record is not applied"; then
    bad "N. the host test no longer pins the stale-replica branch — newer-wins with only its winning direction tested is a comparison that could be inverted unnoticed"
else
    ok "N. the replica is consulted on a hole, ordered by capture seq, promoted through the migration receiver, replayed, and pinned by the host test on both the restore and the fallback"
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "---- env_failover_check: ALL INVARIANTS HOLD"
else
    echo "---- env_failover_check: FAILURES ABOVE"
fi
[ "$fail" -eq 0 ]
