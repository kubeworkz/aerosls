#!/usr/bin/env bash
# tests/selfhosted_routing_smoke.sh — asserts the CI routing record in the
# claim-verification doc matches the actual runs-on routing in ci.yml, so
# the self-hosted runner adoption cannot become permanent by accident.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# On 2026-08-17 GitHub's hosted-runner fleet failed to attach runners to
# jobs (every job died 2-6s in with runner_id=0, zero steps, zero logs;
# see the §0b record in docs/AeroSLS-QEMU-SLS-Claim-Verification-v0.1.md).
# The workflow's five jobs were routed to a self-hosted WSL runner
# (commit 23366da). That routing is a deliberate, dated exception, and the
# failure mode this smoke closes is the exception outliving the incident:
# the hosted fleet recovers quietly — GitHub's status page stayed
# "Operational" throughout the incident — so nothing pages anyone when it
# heals, and an unreverted `[self-hosted, Linux, X64]` runs-on keeps paying
# 35-40 minutes of serial CI per push on a 3.7 GiB VM forever.
#
# The discipline enforced here is a two-way lock between the code and the
# record, so a revert and its record must always move together in one
# commit:
#   * all jobs on [self-hosted, Linux, X64]  ⟹  the doc must carry
#     `CI-ROUTING: SELF-HOSTED — adopted <date> (commit <sha>)`
#   * all jobs on ubuntu-latest              ⟹  the doc must carry
#     `CI-ROUTING: HOSTED — recovered <date> (commit <sha>)`
# A revert that touches only ci.yml fails (the doc still says SELF-HOSTED).
# A doc flip without the revert fails too (the routing is still
# self-hosted). Mixed routing fails outright. So the recovery date is
# recorded at the moment it happens, never reconstructed later.
#
# The GitHub status probe is informational only: it prints the Actions
# component state and, when the fleet reports operational while routing is
# still self-hosted, a reminder that the revert is due. It never fails the
# smoke — the network may be absent, and the status page demonstrably
# lags the fleet (the authoritative recovery signal is a hosted job that
# actually gets a runner, which the §0b doc tells you how to probe).
#
# Source-only: reads ci.yml and the doc, touches nothing. Auto-included by
# run_source_smokes.sh (the verify job's teeth, every push) and by
# run_guard_smokes.sh (build hosts) via the tests/*_smoke.sh glob.
#
# Exit: 0 if routing and record agree, 1 otherwise, 2 on a missing file.
set -u

cd "$(dirname "$0")/.."   # repo root

CI=.github/workflows/ci.yml
DOC=docs/AeroSLS-QEMU-SLS-Claim-Verification-v0.1.md
fails=0

[ -f "$CI" ]  || { echo "ABORT: $CI missing"  >&2; exit 2; }
[ -f "$DOC" ] || { echo "ABORT: $DOC missing" >&2; exit 2; }

# ─── routing state from ci.yml ─────────────────────────────────────────────
selfhosted=$(grep -cF 'runs-on: [self-hosted, Linux, X64]' "$CI" || true)
hosted=$(grep -cF 'runs-on: ubuntu-latest' "$CI" || true)
total=$((selfhosted + hosted))

if [ "$total" -eq 0 ]; then
    echo "FAIL  no runs-on lines found in $CI"
    fails=$((fails + 1))
elif [ "$selfhosted" -gt 0 ] && [ "$hosted" -gt 0 ]; then
    echo "FAIL  mixed routing: $selfhosted self-hosted + $hosted hosted — all jobs must share one fleet"
    fails=$((fails + 1))
fi

# ─── record state from the doc ─────────────────────────────────────────────
record=$(grep -oE 'CI-ROUTING: (SELF-HOSTED|HOSTED)' "$DOC" | head -1 || true)
case "$record" in
    "CI-ROUTING: SELF-HOSTED")
        state=ACTIVE
        date=$(grep -oE 'adopted [0-9]{4}-[0-9]{2}-[0-9]{2}' "$DOC" | head -1 || true)
        [ -n "$date" ] || { echo "FAIL  record says SELF-HOSTED but carries no adoption date" ; fails=$((fails + 1)); }
        ;;
    "CI-ROUTING: HOSTED")
        state=RECOVERED
        date=$(grep -oE 'recovered [0-9]{4}-[0-9]{2}-[0-9]{2}' "$DOC" | head -1 || true)
        [ -n "$date" ] || { echo "FAIL  record says HOSTED but carries no recovery date" ; fails=$((fails + 1)); }
        ;;
    *)
        state=UNRECORDED
        echo "FAIL  CI-ROUTING marker missing from $DOC (must read SELF-HOSTED or HOSTED)"
        fails=$((fails + 1))
        ;;
esac

echo "routing: $selfhosted self-hosted / $hosted hosted  |  record: ${record:-none}"

# ─── the two-way lock ──────────────────────────────────────────────────────
if [ "$total" -gt 0 ] && [ "$selfhosted" -eq "$total" ] && [ "$state" != ACTIVE ]; then
    echo "FAIL  all jobs route to the self-hosted runner but the doc record is $state —"
    echo "      the adoption is an unrecorded exception. Add the CI-ROUTING: SELF-HOSTED"
    echo "      record (with adoption date and commit) before relying on this routing."
    fails=$((fails + 1))
fi

if [ "$total" -gt 0 ] && [ "$hosted" -eq "$total" ] && [ "$state" != RECOVERED ]; then
    echo "FAIL  all jobs route to ubuntu-latest but the doc record is $state —"
    echo "      a revert must record the recovery in the same commit: flip the marker to"
    echo "      CI-ROUTING: HOSTED — recovered <date> (commit <sha>) in"
    echo "      $DOC"
    fails=$((fails + 1))
fi

if [ "$total" -gt 0 ] && [ "$selfhosted" -eq "$total" ] && [ "$state" = RECOVERED ]; then
    echo "FAIL  the doc records the fleet as recovered, but the routing is still self-hosted —"
    echo "      finish the revert: change the five runs-on lines in $CI back to ubuntu-latest."
    fails=$((fails + 1))
fi

# ─── informational fleet probe (never fails the smoke) ─────────────────────
# githubstatus.com's v2 API exposes only page + status (no component list),
# so the readable signal is status.indicator / status.description. The status
# page demonstrably lags the fleet (it read Operational throughout the
# incident), so this is informational only: the authoritative recovery signal
# is a hosted job that actually gets a runner — see the §0b doc record.
probe_fleet() {
    command -v curl >/dev/null 2>&1 || return 0
    command -v python3 >/dev/null 2>&1 || return 0
    out=$(curl -fsS --max-time 8 https://www.githubstatus.com/api/v2/status.json 2>/dev/null) || return 0
    state=$(printf '%s' "$out" | python3 -c 'import sys,json
try:
    d=json.load(sys.stdin)
    s = d.get("status", {}) or {}
    print("%s (%s)" % (s.get("indicator", "?"), s.get("description", "?")))
except Exception:
    pass' 2>/dev/null)
    [ -n "$state" ] || return 0
    case "$state" in
        "none ("*)
            if [ "$total" -gt 0 ] && [ "$selfhosted" -eq "$total" ]; then
                echo "REMINDER  GitHub status reports All Systems Operational while routing is still"
                echo "          self-hosted. The status page lags the fleet — the authoritative"
                echo "          recovery signal is a hosted job that actually gets a runner (see the"
                echo "          §0b doc probe). Once one has, the revert is due: five runs-on lines"
                echo "          to ubuntu-latest + the CI-ROUTING: HOSTED flip, in one commit."
            else
                echo "fleet status: $state"
            fi
            ;;
        *)
            echo "fleet status: $state"
            ;;
    esac
}
probe_fleet

[ "$fails" -eq 0 ]
