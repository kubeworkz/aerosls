#!/usr/bin/env bash
# tests/runner_host_keep_awake_smoke.sh -- proves tests/runner_host_keep_awake.sh's
# gap rule and its preflight seams bite: jitter and the exact limit are normal,
# one second past the limit is a suspend, a backwards step is classified as one,
# a resume inside the window warns while an old one does not, an enter with no
# exit is not a resume, recent suspend evidence warns while old evidence does
# not, a Lid-close Reason is surfaced with its remedy while another Reason is
# not, the arm payload decodes to the real SetThreadExecutionState call, the
# events payload reads each event's Message for the Reason, and a host with no
# Windows bridge skips instead of failing.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# runner_host_keep_awake.sh decides, from two numbers, whether the runner host
# stopped running -- the suspend signature that killed kernel-guards in run
# 36270544785 (2026-09-26: Modern Standby at 21:46:44Z, the WSL VM frozen
# 21 minutes, the job expired service-side and cancelled on resume). It also
# reports WHY the standby happened, from the Kernel-Power Reason: the
# 2026-09-27 case (run 36350908094) died the same way with Reason: Lid, which
# the keep-awake power request cannot prevent -- so the reason is named and the
# remedy (keep the lid open) is printed. The smoke pins both directions: a Lid
# warns and prints the remedy, another reason does not. A rule
# like that fails in both directions: too loose and a suspend reads as a
# normal job, too eager and every loaded host warns on itself. The boundaries
# (at the slack, one second past it) are pinned here, not just the extreme
# case, because the whole value of the number is at the boundary.
#
# The resume/evidence windows are driven through the documented test seams
# (fixtures in $T, a fixed --now-epoch), so the smoke is hermetic: it never
# reads the live Windows event log, never arms a power request, and never
# writes the shared /tmp evidence. The fixture line format is exactly what the
# live Get-WinEvent query prints ("<iso-utc> <id> <reason>"), so the parser
# under test is the parser the runner runs; the 2026-09-26/27 pairs are
# reproduced here as timestamped fixture data, not as a special case.
#
# Source-only: needs only the tree, GNU date and base64/iconv (coreutils). No
# build, no QEMU, so it runs in the verify job's run_source_smokes.sh on every
# push, alongside the build-host run_guard_smokes.sh pass.
#
# ─── Measured (2026-09-26, on this host) ──────────────────────────────────
#   baseline (no events, no evidence)      -> warnings=0
#   jump 15/16 (jitter)                    -> gap=1s verdict=normal
#   jump 15/1375 (the real signature)      -> gap=1360s verdict=suspend
#   jump 15/75 (at the limit)              -> gap=60s verdict=normal
#   jump 15/76 (one second past)           -> gap=61s verdict=suspend
#   jump 15/5  (small back-drift)          -> gap=-10s verdict=normal
#   jump 15/-45 (back-step at the limit)   -> gap=-60s verdict=normal
#   jump 15/-46 (one second past, back)    -> gap=-61s verdict=step-back
#   resume 181s ago (window 600)           -> 1 warning, names the age
#   resume 4000s ago                       -> warnings=0
#   enter-only (506/566, no 507)           -> warnings=0
#   evidence: suspend 20 min ago           -> 1 warning, names the job + gap
#   evidence: suspend 25 h ago             -> warnings=0
#   evidence: step back 10 min ago         -> 1 warning, clock-step wording
#   payload decoded                        -> SetThreadExecutionState + 2147483649
#   events payload decoded                 -> Kernel-Power query reads each Message
#   lid: resume 181s ago, reason Lid       -> 2 warnings: resume + lid remedy
#   power button: resume 181s ago          -> 1 warning, reason named, no lid
#   no-reason event line (2 fields)        -> reason=none lid=0, still parses
#   start --no-bridge                      -> SKIP, exit 0
#
# Exit: 0 if every tooth bit, 1 otherwise.
set -u
cd "$(dirname "$0")/.."   # repo root, so the helper path resolves

KEEP=tests/runner_host_keep_awake.sh
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT

teeth=0
fails=0
ok()  { teeth=$((teeth + 1)); echo "PASS  $1"; }
bad() { fails=$((fails + 1)); echo "FAIL  $1"; }
has() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

# ─── fixtures: the live query's line format, at a fixed now ────────────────
NOW=1790000000
fixture_event() {  # fixture_event EPOCH ID -> "<iso-utc> <id>"
    printf '%s %s\n' "$(date -u -d "@$1" +%Y-%m-%dT%H:%M:%SZ)" "$2"
}
fixture_event_reason() {  # EPOCH ID REASON -> "<iso-utc> <id> <reason>"
    printf '%s %s %s\n' "$(date -u -d "@$1" +%Y-%m-%dT%H:%M:%SZ)" "$2" "$3"
}
fixture_event "$((NOW - 181))"  507 > "$T/recent.events"
fixture_event "$((NOW - 1500))" 506 >> "$T/recent.events"
fixture_event "$((NOW - 4000))" 507 > "$T/old.events"
fixture_event "$((NOW - 1500))" 506 > "$T/enter-only.events"
fixture_event "$((NOW - 1498))" 566 >> "$T/enter-only.events"
# A lid close forces Modern Standby with Reason: Lid on both the 506 enter and
# the 507 exit -- the shape the 2026-09-27 host showed. A Power Button pair is
# the control that must NOT claim a lid.
fixture_event_reason "$((NOW - 1500))" 506 Lid          >  "$T/lid.events"
fixture_event_reason "$((NOW - 181))"  507 Lid          >> "$T/lid.events"
fixture_event_reason "$((NOW - 1500))" 506 Power_Button >  "$T/powerbutton.events"
fixture_event_reason "$((NOW - 181))"  507 Power_Button >> "$T/powerbutton.events"
: > "$T/none.events"
printf 'SUSPENDED %s gap=1323s job=kernel-guards\n' \
    "$(date -u -d "@$((NOW - 1200))" +%Y-%m-%dT%H:%M:%SZ)" > "$T/evidence-recent.log"
printf 'SUSPENDED %s gap=1323s job=kernel-guards\n' \
    "$(date -u -d "@$((NOW - 90000))" +%Y-%m-%dT%H:%M:%SZ)" > "$T/evidence-old.log"
printf 'STEPBACK %s gap=5s job=verify\n' \
    "$(date -u -d "@$((NOW - 600))" +%Y-%m-%dT%H:%M:%SZ)" > "$T/evidence-stepback.log"

check_out() {  # check_out EVENTS_FILE EVIDENCE_FILE -> preflight output
    bash "$KEEP" check --no-bridge --now-epoch "$NOW" \
        --events-file "$1" --evidence-file "$2" 2>&1
}

expect_verdict() {  # expected observed want label
    local got
    got="$(bash "$KEEP" jump "$1" "$2" 2>&1)"
    if has "$got" "verdict=$3"; then
        ok "$4 ($got)"
    else
        bad "$4: wanted verdict=$3, got '$got'"
    fi
}

# ── 1. the clean baseline: nothing to report, and no warning words ─────────
out="$(check_out "$T/none.events" "$T/absent.log")"
if has "$out" 'preflight: warnings=0' && ! has "$out" 'WARN'; then
    ok "baseline (no events, no evidence) -> warnings=0"
else
    bad "baseline was not clean:"; printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 2-6. the gap rule, at the boundaries ───────────────────────────────────
expect_verdict 15 16   normal    "jitter (15/16) is normal"
expect_verdict 15 5    normal    "a small back-drift (15/5, -10s) is inside the slack"
expect_verdict 15 1375 suspend   "the real signature (15/1375) is a suspend"
expect_verdict 15 75   normal    "at the slack limit (15/75) is normal"
expect_verdict 15 76   suspend   "one second past the limit (15/76) is a suspend"
expect_verdict 15 -45  normal    "a back-step at the slack limit (15/-45) is normal"
expect_verdict 15 -46  step-back "one second past it backwards (15/-46) is step-back"

# ── 7. a resume inside the window warns, and the run still exits 0 ─────────
out="$(check_out "$T/recent.events" "$T/absent.log")"
rc=$?
if [ "$rc" = 0 ] && has "$out" "WARN  host resumed from standby 181s ago" \
    && has "$out" 'preflight: warnings=1' && has "$out" 'preflight: source=file'; then
    ok "a resume 181s ago warns once (event named, exit still 0)"
else
    bad "recent resume did not warn as expected (rc=$rc):"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 8. an old resume does not warn: the window is a window ─────────────────
out="$(check_out "$T/old.events" "$T/absent.log")"
if has "$out" 'preflight: warnings=0' && ! has "$out" 'WARN'; then
    ok "a resume 4000s ago is outside the 600s window -> warnings=0"
else
    bad "old resume warned:"; printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 9. an enter is not a resume: only 507/107 count ────────────────────────
out="$(check_out "$T/enter-only.events" "$T/absent.log")"
if has "$out" 'resumes=0 unparsed=0' && has "$out" 'preflight: warnings=0'; then
    ok "enter-only events (506/566) are not a resume -> warnings=0"
else
    bad "enter-only events were misread:"; printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 10-12. the evidence file: what the next job says about the last one ────
out="$(check_out "$T/none.events" "$T/evidence-recent.log")"
if has "$out" "records a suspend gap during job 'kernel-guards'" \
    && has "$out" 'gap=1323s' && has "$out" 'preflight: warnings=1'; then
    ok "recent suspend evidence warns once, naming the job and the gap"
else
    bad "recent suspend evidence did not warn as expected:"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

out="$(check_out "$T/none.events" "$T/evidence-old.log")"
if has "$out" 'preflight: warnings=0' && ! has "$out" 'WARN'; then
    ok "suspend evidence older than the window -> warnings=0"
else
    bad "old suspend evidence warned:"; printf '%s\n' "$out" | sed 's/^/      /'
fi

out="$(check_out "$T/none.events" "$T/evidence-stepback.log")"
if has "$out" 'records a backwards clock step' && has "$out" 'gap=5s'; then
    ok "a step-back record uses the clock-step wording, not the suspend one"
else
    bad "step-back evidence was misread:"; printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 13. the payload the arm path sends really is the power-request call ────
enc="$(bash "$KEEP" payload 2>/dev/null | head -n 1)"
ps_text="$(printf '%s' "$enc" | base64 -d 2>/dev/null | iconv -f UTF-16LE -t UTF-8 2>/dev/null || true)"
if has "$ps_text" 'SetThreadExecutionState' && has "$ps_text" '2147483649' \
    && has "$ps_text" 'Start-Sleep -Seconds 7200' && has "$ps_text" '2147483648'; then
    ok "payload decodes to the arm script (ES_CONTINUOUS|ES_SYSTEM_REQUIRED, 7200s hold, release)"
else
    bad "payload did not decode to the arm script (${#enc} base64 chars):"
    printf '%s\n' "$ps_text" | sed 's/^/      /'
fi

# ── 14. a foreign host skips: the mitigation never fails a job ─────────────
out="$(AEROSLS_KEEP_AWAKE_LOG="$T/keep.log" bash "$KEEP" start --no-bridge \
    --now-epoch "$NOW" --events-file "$T/none.events" --evidence-file "$T/absent.log" 2>&1)"
rc=$?
if [ "$rc" = 0 ] && has "$out" 'keep-awake: SKIP' && ! has "$out" 'WARN'; then
    ok "start on a host with no Windows bridge prints SKIP and exits 0"
else
    bad "start --no-bridge did not skip cleanly (rc=$rc):"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 15. a Lid reason is surfaced, with the remedy, alongside the resume ─────
out="$(check_out "$T/lid.events" "$T/absent.log")"
rc=$?
if [ "$rc" = 0 ] && has "$out" 'reason=Lid lid=1' \
    && has "$out" 'host resumed from standby 181s ago (event 507' \
    && has "$out" 'reason=Lid' && has "$out" 'was for Reason: Lid' \
    && has "$out" 'keep the lid OPEN' && has "$out" 'preflight: warnings=2'; then
    ok "a Lid-close transition warns twice: the resume plus the lid remedy"
else
    bad "a lid reason was not surfaced as expected (rc=$rc):"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 16. the lid rule is specific: another reason names itself, no lid claim ──
out="$(check_out "$T/powerbutton.events" "$T/absent.log")"
if has "$out" 'reason=Power_Button lid=0' && has "$out" 'reason=Power_Button' \
    && ! has "$out" 'keep the lid OPEN' && has "$out" 'preflight: warnings=1'; then
    ok "a non-lid reason (Power_Button) is named but does not claim a lid"
else
    bad "a non-lid reason was misread:"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 17. a two-field line (the old format, no reason) still parses ──────────
out="$(check_out "$T/recent.events" "$T/absent.log")"
if has "$out" 'reason=none lid=0' && has "$out" 'preflight: warnings=1'; then
    ok "a two-field event line (no reason) parses as reason=none lid=0"
else
    bad "a reason-less line broke the parser:"
    printf '%s\n' "$out" | sed 's/^/      /'
fi

# ── 18. the events payload the live preflight sends fetches the Message ────
enc="$(bash "$KEEP" events-payload 2>/dev/null | head -n 1)"
ps_text="$(printf '%s' "$enc" | base64 -d 2>/dev/null | iconv -f UTF-16LE -t UTF-8 2>/dev/null || true)"
if has "$ps_text" 'Microsoft-Windows-Kernel-Power' && has "$ps_text" '$_.Message' \
    && has "$ps_text" 'Reason:' && has "$ps_text" '.Id'; then
    ok "events payload queries Kernel-Power and reads each event's Message/Reason"
else
    bad "events payload did not carry the reason query (${#enc} base64 chars):"
    printf '%s\n' "$ps_text" | sed 's/^/      /'
fi

echo
echo "runner_host_keep_awake_smoke: $teeth teeth bit, $fails failed"
[ "$fails" -eq 0 ]
