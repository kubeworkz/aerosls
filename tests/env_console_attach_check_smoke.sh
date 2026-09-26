#!/usr/bin/env bash
# tests/env_console_attach_check_smoke.sh — proves
# tests/env_console_attach_check.sh has teeth: pointed at inputs that do NOT
# have the E6 property, the guard must FAIL (not pass quietly, and not hang).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The E6 guard's subject is an ABSENCE assertion — "each stream contains only
# its own output" — plus the census of the two consoles. Absence assertions have
# a failure mode that counts do not: two empty streams satisfy "neither stream
# carries the other's output" perfectly, and a guard that had drifted into
# checking exactly that would report PASS on an environment nothing can reach,
# which is the defect E6 exists to fix (G7). And the census assertions have the
# mirror image: they hold for a registry whose two entries are swapped with each
# other, because a mutual swap preserves every count while mis-labelling every
# address. The arms below are chosen against those two failure modes, not
# against the property in general — and the second of those failure modes is now
# covered by an assertion of the guard's own rather than only argued about: the
# environment's own boot announcement of its index and pid (guard clause 3b,
# sidecar boot.rs), control C below.
#
# The arms need no second kernel build (which is why they run on every
# kernel-guards pass); the kernel-level mutants that DO is why the source
# controls below are a record rather than an arm.
#
# Eight arms, seven of them teeth, listed in the order the code below runs them
# (the prerequisite arm first, so a source-only runner is told what is missing
# before anything is booted). Six of the seven teeth require the guard to go RED
# on an input that lacks the property; the seventh (arm 6) is a TIMING tooth and
# requires the guard to stay GREEN on an input that has the property but delivers
# it late — see arm 6 for why a late line is a tooth and not a nicety.
#
#   1. PREREQUISITE CONTRACT — E6_ISO pointing at nothing must exit 2 and say
#      why. run_checks.sh files a runtime guard's 2 as an owed skip, so a guard
#      that "passed" a missing ISO would be a guard nobody runs.
#
#   2. TOOTH (wrong boot) — E6_BOOT_ENTRY=2 boots grub's kernel-only entry,
#      which has the kernel HTTP server but NO unified control plane and NO
#      init, so there is no environment manager to drive: the guard must fail,
#      and its own contradiction check names it early.
#
#   3. TOOTH (cross-wire) — E6_TOOTH=cross-wire runs the guard on the REAL
#      unified boot but delivers every command to the OTHER environment's
#      console. This is the roadmap §9 tooth ("cross-wire the two consoles —
#      the isolation assertion fails") reproduced without a second kernel
#      build, and it is the input that the absence assertions exist for: both
#      environments are live, both consoles answer, both streams have content —
#      only the content is the wrong environment's. The guard must go red on
#      the isolation clause. A guard that only asked "did attach see output"
#      passes this arm, and the smoke fails it for that.
#
#   4. TOOTH (no input) — E6_TOOTH=no-input runs the real boot and withholds
#      every command. Both streams stay empty, so every ABSENCE assertion in the
#      guard holds; only the own-output assertion can fail. The guard must go
#      red on it. This arm is the reason the guard asserts its own marker rather
#      than merely the other one's, and it is the arm that would catch a guard
#      whose marker check had been weakened to "the stream is non-empty" or
#      dropped entirely.
#
#   5. TOOTH (no pause) — E6_TOOTH=skip-pause runs the real boot and leaves the
#      guard's neighbour partition LIVE. Phase 6b rests on a frozen peer; with
#      the peer still draining, the paused-peer clauses must fail. This arm is
#      checked BOTH ways (arm_lacks): the clauses that went red have to be the
#      pause-dependent ones and the rest of the guard — the census, the
#      attribution tie, the identity clauses, phase A's stream clauses — has to
#      stay green, or the arm would be satisfied by the guard collapsing for some
#      unrelated reason.
#
#   6. TOOTH (late identity) — E6_TOOTH=late-identity runs the real boot and
#      holds environment 2's own boot announcement out of the guard's read for
#      E6_LATE_POLLS polls (default 3) before releasing it. The line is LATE, not
#      missing, so the guard must still PASS — and it must pass for the right
#      reason, because its wait is for BOTH streams to announce rather than for
#      either one. Why this is a tooth at all: kernel-guards run 36161966465 went
#      red on clause 3b with no mutant in the tree, and the failing line was
#      "environment 2's console does not carry its own announcement ... the
#      '[env-id]' line it did carry: " (empty) — the guard had broken its wait as
#      soon as EITHER stream announced and then asserted BOTH against a stream it
#      had not finished reading. An invisible-until-it-isn't race is not a tooth;
#      this arm makes it deterministic by construction instead of by luck.
#
#   7. TOOTH (identity withheld) — E6_TOOTH=late-identity-gone withholds that
#      line for good, so arm 6's wait has to be shown to be a wait and not an
#      early exit. With E6_ATTACH_WAIT_S=6 the guard must go red on clause 3b's
#      bounded-wait message — and on THAT message and not the per-stream one,
#      which is what arm_lacks below checks — after taking its full window.
#      Together, arms 6 and 7 pin the wait to the right event: a lost line fails,
#      a late line does not.
#
#   8. RESTORE — the guard unmodified on the unified boot must pass, so the
#      smoke also proves the guard is not simply failing everything (a tooth
#      that fires on both inputs proves nothing about either).
#
# ─── The SOURCE controls, and what each one measured ───────────────────────
# The roadmap's tooth asks for a cross-wired pair of consoles, and building one
# means a second kernel build — which is the boundary the E4 and E5 smokes drew
# for the same reason (the arms above deliberately need no build, so they run on
# every kernel-guards pass). The kernel-level mutants were therefore run by hand,
# one per assertion family, and are recorded here with their measured verdicts,
# because each one answers a different question: which assertion fires (A, B, C,
# D and E — one per assertion family, in that order; C was the one that fired
# nowhere until the in-band identity was added and is recorded both ways — its
# older measurement and the re-measurement that closed it — and E is the one the
# paused-neighbour phase exists to catch). Each is a
# one-place edit in kernel/env_console.c; the ISO the guard boots is rebuilt with
# it in place, copied aside, and the source reverted, so every control is
# reproducible without touching the shipped image (E6_ISO is the knob):
#
#   * CONTROL A (cross-wire at registration) — `ec_parse_index()` swaps indexes
#     1 and 2, so each sidecar's channel ends are filed under the other
#     environment's index. This is literally "the two consoles are cross-wired":
#     the wire from sidecar 1 lands in environment 2's console and vice versa.
#     Measured rc=1, and the assertion that carries it is the CENSUS, before any
#     attach: `FAILED: GET /api/partition/1/env never reported both
#     environments' consoles (live='2', wanted 2; env 1 index='', env 2
#     index='2')`. The mechanism is in the kernel's own log — init's ENV_CREATE
#     reply for environment 1 arrives while the only console that exists is
#     filed under index 2, so the binding finds nothing:
#     `[ENV] environment id 1 (partition 1, index 1) has no console to bind —
#     attach will answer no such environment`. An environment that cannot be
#     addressed by id is exactly G7's unreachable environment, and the census
#     sees it without typing anything.
#
#   * CONTROL B (one half crossed) — `env_console_read()` hands back the other
#     live environment's buffer in the same partition while the write half stays
#     correct. Measured rc=1, phase A red on all four stream clauses: both
#     consoles `never delivered its own command's output` AND both `delivered
#     environment N's output ... the two consoles are NOT isolated`. This is the
#     shape arm 2 reproduces at the attach layer, and this control is what shows
#     the guard's reading of it is not an artifact of the guard's own request
#     sequence.
#
#   * CONTROL D (attribution misreported) — `env_console_entry()` reports the
#     sibling environment's `posix_pid` in the listing. Nothing about which
#     sidecar a console really belongs to changes, so the streams are CLEAN
#     under this mutant; measured rc=1 on the identity tie alone: `FAILED: the
#     console registry and /api/processes disagree about which sidecar owns a
#     console (env 1 -> index '1' pid '106' name 'aerosls.posix.2'; env 2 ->
#     index '2' pid '104' name 'aerosls.posix.1')` with phase A still green
#     afterwards. That is the assertion an operator's own reading of the listing
#     depends on, and it is the one that fires when the listing lies.
#
#   * CONTROL C (both halves crossed, consistently) — `ec_find_env()` returns a
#     sibling console in the same partition whenever the addressed environment
#     has one, in both directions at once, so every byte addressed to 1 goes to
#     2's sidecar and every byte read back from 1 comes from it. (The map is
#     restricted to BOUND environments, so a destroyed environment's address
#     still answers 0 and the guard's later phases are not broken by the swap
#     itself — otherwise the control would be testing phase 8, not the residual.)
#
#       MEASURED BEFORE the in-band identity existed: rc=0, the guard PASSES.
#     A mutual swap preserved every observable the attach surface offered — the
#     shell's variables, its filesystem and its output all followed the address
#     consistently — and the guard's own header said so. That measurement stood
#     as this guard's stated limit.
#
#       RE-MEASURED NOW, on the same mutant and the same guard with clause 3b
#     added: rc=1, red on the identity clause ALONE. The four failing lines are
#     exactly the four identity clauses (each stream's own announcement absent,
#     and each stream carrying the other's), with the census, the attribution
#     tie and phase A's four stream clauses all still GREEN under the mutant —
#     the swap really is consistent, and the announcement is the only thing that
#     says so. The transcript below is one boot's (the pids in it are that
#     boot's; the failure is not):
#         FAILED: identity: environment 1's console does not carry its own
#           announcement (expected '[env-id] index=1 pid=104'; the '[env-id]'
#           line it did carry: [env-id] index=2 pid=106) — the stream an operator
#           reads at environment 1's address belongs to another environment
#         FAILED: identity: environment 2's console does not carry its own
#           announcement (expected '[env-id] index=2 pid=106'; ... [env-id]
#           index=1 pid=104)
#         FAILED: identity: environment 1's console carries environment 2's
#           announcement ('[env-id] index=2 pid=106') — the two consoles are
#           swapped
#         FAILED: identity: environment 2's console carries environment 1's
#           announcement ('[env-id] index=1 pid=104') — the two consoles are
#           swapped
#     The reason it cannot be hidden is structural, not a matter of how much is
#     typed: the announcement is emitted by the sidecar on its own output
#     channel and the kernel files it under the entry that channel belongs to,
#     while a reader reaches a console by (partition, env_id) — the claim
#     travels by the path the mutant left alone and is read by the path it
#     rewrote. Source reverted byte-identically afterwards (sha256 of
#     kernel/env_console.c unchanged, and the shipped ISO rebuilt without it).
#
#     Recorded in this much detail because a tooth's honest limit is worth more
#     than the appearance of one, and because this is the limit that was closed.
#
#   * CONTROL E (partition-blind console registry) — `ec_find_env()` matches on
#     `env_id` alone instead of (partition, env_id), which is the plausible way
#     "two environments in different partitions are not reachable through each
#     other's addresses" could actually break: the registry stops being scoped,
#     so an address that names one partition resolves to an environment in
#     another. Measured rc=1, and the five failing lines are exactly the
#     cross-partition family of guard clause 6b and nothing else — the four
#     probes, each answering instead of refusing, and the delivery marker that
#     shows the leaked write was not just answered but RUN:
#         FAILED: GET /api/partition/1/env/3/console answered ok='true' — env 3
#           is not an environment of partition 1 and must not be reachable
#           through its address (body: {"ok":"true","env_id":3,...})
#         FAILED: POST /api/partition/1/env/3/console answered ok='true'
#           (queued='14') — input must not be deliverable across partitions'
#           addresses
#         FAILED: GET /api/partition/2/env/1/console answered ok='true' — ...
#         FAILED: POST /api/partition/2/env/1/console answered ok='true'
#           (queued='14') — ...
#         FAILED: 'hi-cross' appeared in a stream — a write the control plane
#           refused as a cross-partition address was delivered to an environment
#           anyway (the address matched an environment outside the partition it
#           named)
#     Under the same mutant the census, the attribution tie, the identity
#     clauses (3b), phase A's four stream clauses and the WHOLE pause family of
#     clause 6b stayed green — so the paused-neighbour clauses and the
#     cross-partition clauses are not two ways of saying one thing: the pause
#     family is what arm 4 (skip-pause) moves, and this family is what control E
#     moves, in both cases alone. Source reverted byte-identically (sha256 of
#     kernel/env_console.c back to 2a7514c5..., diff vs the pristine copy empty)
#     and the shipped ISO rebuilt from it.
#
# Needs the built ISO + QEMU, so it degrades to the prerequisite-contract arm on
# a source-only runner (and prints why); on a build host it runs all eight arms,
# via run_guard_smokes.sh and CI's kernel-guards job.
#
# (Arms 6 and 7 are a pair around one line's arrival: the late line must be
# absorbed and the withheld line must not be, so the guard's wait is pinned to the
# event it claims to wait for. Arm 6 is the only arm that expects the guard's own
# verdict to be PASS on a tooth input — the smoke still fails if it goes red.)
#
# Exit: 0 when every tooth fired and the real input still passed, 1 otherwise,
# 2 on a missing prerequisite.
set -u

cd "$(dirname "$0")/.."   # repo root

ISO="${E6_SMOKE_ISO:-sls_operating_system.iso}"
GUARD=tests/env_console_attach_check.sh

[ -f "$GUARD" ] || { echo "ABORT: $GUARD missing" >&2; exit 2; }

fail=0
# arm_says <needle> — the last arm's output must ALSO carry this verdict text.
# Separate from run_arm because the verdict alone is not enough: several
# different failures exit 1, and a tooth that only checked "it went red" would
# accept the guard failing for any reason, including a boot that never came up.
last_out=""
arm_says() {   # arm_says <needle> <what-it-means>
    if printf '%s' "$last_out" | grep -qF "$1"; then
        echo "      and it said why: $2"
        # The guard's own wording, not this file's paraphrase of it: the point of
        # the tooth is that the guard says WHICH clause failed, so the clause is
        # quoted from its own output.
        printf '%s\n' "$last_out" | grep -F "$1" | head -2 | sed 's/^/        /'
    else
        echo "FAIL: the guard went red without saying '$1' ($2)" >&2
        printf '%s\n' "$last_out" | tail -8 | sed 's/^/      /' >&2
        fail=1
    fi
}
# arm_lacks <needle> <what-its-absence-means> — the last arm's output must NOT
# carry this text. The complement of arm_says, and the reason arm 4 is evidence:
# a tooth proves something only if the clauses that move are the ones the tooth
# is about, so the smoke checks that the rest of the guard stayed standing.
arm_lacks() {   # arm_lacks <needle> <what-the-absence-means>
    if printf '%s' "$last_out" | grep -qF "$1"; then
        echo "FAIL: the guard's output also carried '$1' ($2)" >&2
        printf '%s\n' "$last_out" | grep -F "$1" | head -2 | sed 's/^/      /' >&2
        fail=1
    else
        echo "      and nothing else moved: no '$1' ($2)"
    fi
}
run_arm() {   # run_arm <verdict: pass|fail|abort> <label> <env...> — keeps the output
    last_out=""
    local want="$1" label="$2"; shift 2
    local rc
    last_out="$(env "$@" timeout 900 bash "$GUARD" 2>&1)"; rc=$?
    local got
    case "$rc" in
        0) got=pass ;;
        2) got=abort ;;
        *) got=fail ;;
    esac
    if [ "$got" = "$want" ]; then
        echo "ok:   $label (verdict: $got, exit $rc)"
    else
        echo "FAIL: $label — wanted '$want', got '$got' (exit $rc)" >&2
        printf '%s\n' "$last_out" | tail -12 | sed 's/^/      /' >&2
        fail=1
    fi
}

# ── Arm 1: the prerequisite contract (no ISO needed; runs anywhere) ────────
run_arm abort "a missing ISO is refused (exit 2), never a quiet pass" \
    E6_ISO=/nonexistent/aerosls_e6_smoke.iso
arm_says "missing" "it says which prerequisite it wanted, so run_checks files it as an owed skip and not as rot"

if [ ! -f "$ISO" ] || ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "note: $ISO or qemu-system-x86_64 missing — the seven booting arms cannot run here" >&2
    [ "$fail" -eq 0 ] && echo "note: prerequisite contract only (this is a build host smoke)"
    exit "$fail"
fi

# ── Arm 2: TOOTH — the wrong boot has no environment manager at all ────────
run_arm fail "the kernel-only boot (entry 2) fails the guard — no environment manager exists there" \
    E6_ISO="$ISO" E6_BOOT_ENTRY=2 E6_WINDOW_S=90
arm_says "FAILED:" "the guard names the boot it could not drive"

# ── Arm 3: TOOTH — cross-wire the consoles; the streams change hands ───────
# A generous attach window on purpose: the point of this arm is that the guard
# goes red on the ISOLATION clause while both consoles are answering, so the
# crossed output has to have arrived. A window too short to see it would fail
# the arm for the wrong reason (own output missing) and the arm_says check below
# is what refuses to accept that.
run_arm fail "cross-wiring the two consoles turns the guard red" \
    E6_ISO="$ISO" E6_TOOTH=cross-wire E6_ATTACH_WAIT_S=30
arm_says "the two consoles are NOT isolated" "both streams are answering, and each carries the other's output"

# ── Arm 4: TOOTH — withhold the input; only emptiness is left to assert ─────
run_arm fail "withholding every command turns the guard red (two empty streams are not isolation)" \
    E6_ISO="$ISO" E6_TOOTH=no-input E6_ATTACH_WAIT_S=15
arm_says "never delivered its own command's output" "the own-output clause is what caught it, not a foreign byte"

# ── Arm 5: TOOTH — leave the neighbour live; its queue drains, so it is ─────
# not a frozen peer and the clauses that need one must fail. Deliberately not
# given a short attach window: this arm is about the pause, and the rest of the
# guard has to hold for the arm to mean anything (the two arm_lacks checks).
run_arm fail "leaving the neighbour partition live (no pause) turns the guard red on the paused-peer clauses" \
    E6_ISO="$ISO" E6_TOOTH=skip-pause E6_ATTACH_WAIT_S=20
arm_says "DRAINED its queue" "the frozen-peer clause is what caught it: the queue was drained by an environment that was still running"
arm_lacks "the two consoles are NOT isolated" "the same-partition isolation clauses stayed green — this tooth is about the partition boundary, not about console crossing"
arm_lacks "FAILED: identity:" "clause 3b stayed green — the environments still announce themselves correctly"
arm_lacks "never reported both environments' consoles" "the census stayed green — the tooth moved the pause clauses, not the boot"

# ── Arm 6: TOOTH — a LATE identity line must still be absorbed ─────────────
# The one tooth here that must be GREEN, and the only one whose input is a timing
# rather than a wrongness: environment 2 announces, but not until the guard has
# already read its stream a few times. A guard that broke its wait on the first
# '[env-id]' seen anywhere would assert environment 2's line before it arrived
# and go red here; the real guard waits for both streams and passes. This is the
# shape run 36161966465's red had, minus the luck. No shortened attach window on
# purpose: the boot is the real one, and only the arrival of one line is moved.
run_arm pass "a LATE identity line is absorbed (the wait is for both streams, not either)" \
    E6_ISO="$ISO" E6_TOOTH=late-identity
arm_says "holding environment 2's announcement" "the guard says it is running the tooth, so this arm is not the pristine boot by accident"
arm_says "PASS  E6 attach" "and it passed, having waited for the line rather than asserting before it arrived"

# ── Arm 7: TOOTH — withhold it for good; the SAME clause must go red ───────
# The complement of arm 6, and the reason arm 6 is evidence: a guard that had
# quietly stopped checking the announcement would pass arm 6 for the wrong reason
# and would have to pass this one too. The short window is deliberate — the red
# has to come from the bounded wait, and the message has to name what each stream
# carried (environment 2's: nothing), because that line is all an operator has.
run_arm fail "withholding environment 2's identity for good turns clause 3b red, after its bounded wait" \
    E6_ISO="$ISO" E6_TOOTH=late-identity-gone E6_ATTACH_WAIT_S=6
arm_says "never both announced an identity" "the failure is the wait timing out, not an early exit"
arm_says "environment 2's carried nothing" "and it says which stream was silent, so the red is diagnosable from the log alone"
arm_lacks "does not carry its own announcement" "the per-stream clause never ran — the wait is what failed, so arm 6 and this arm are about the same event from both sides"

# ── Arm 8: RESTORE — the guard unmodified, at its own defaults ─────────────
run_arm pass "the guard unmodified passes on the unified boot (the teeth are not blanket failures)" \
    E6_ISO="$ISO"
arm_says "PASS  E6 attach" "it passed on the property, not by skipping the phases"

[ "$fail" -eq 0 ] && echo "ALL PASS: env_console_attach_check.sh's teeth bite and the real input still passes"
exit "$fail"
