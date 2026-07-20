#!/usr/bin/env bash
# monitor/health_check.sh -- Operational Phase C
# (docs/AeroSLS-Operational-MVP-Roadmap-v0.1.md).
#
# Meant to run on a schedule (cron, every 1-5 min) against the PUBLIC URL --
# this checks "is the site actually up for a real user," through
# nginx/Cloudflare and everything else in the chain, which is a different
# question from deploy.sh's own health check (that one deliberately hits
# the kernel directly on localhost:3001, to verify a deploy in isolation).
#
# Does two things a plain uptime monitor (UptimeRobot, Better Uptime, etc.)
# won't do out of the box:
#   1. Alerts on a non-200 or unreachable response, same as any uptime
#      monitor would.
#   2. Alerts if uptime_ticks hasn't INCREASED since the last check -- this
#      catches a wedged-but-still-answering process (accepts connections,
#      returns 200, but its own internal loop has stalled), which a plain
#      "did I get a 200" check can't distinguish from healthy. A DROP in
#      uptime_ticks isn't itself alarming -- that's just what a legitimate
#      restart/deploy looks like -- so this only alerts on a value that's
#      unchanged, not one that's lower.
#
# No external dependencies beyond curl -- the health JSON
# ({"status":"ok","system":"...","uptime_ticks":N,"object_count":N}) is
# simple enough that a plain grep/sed extraction is more appropriate here
# than adding a jq dependency for one integer field.
set -u

HEALTH_URL="${HEALTH_URL:-https://aerosls.kubeworkz.io/api/health}"
STATE_FILE="${STATE_FILE:-/var/lib/aerosls/health_check_state}"
# POSTs a small JSON payload here on any alert, if set -- point this at a
# Slack/Discord "incoming webhook" URL, or any endpoint that'll accept a
# POST. Left unset by default; without it this script still logs to
# stdout/stderr, which cron will email to the local mail user on any
# non-zero exit (cron's own built-in behavior, zero config needed as a
# fallback -- though whether that mail actually reaches anyone depends on
# the server having outbound mail configured at all, worth confirming
# separately rather than assumed).
ALERT_WEBHOOK_URL="${ALERT_WEBHOOK_URL:-}"

alert() {
    local msg="$1"
    echo "[health_check] ALERT: $msg" >&2
    if [ -n "$ALERT_WEBHOOK_URL" ]; then
        curl -sf --max-time 5 -X POST -H "Content-Type: application/json" \
            -d "{\"text\":\"AeroSLS health check: $msg\"}" \
            "$ALERT_WEBHOOK_URL" >/dev/null 2>&1 || \
            echo "[health_check] (also failed to POST to ALERT_WEBHOOK_URL)" >&2
    fi
}

mkdir -p "$(dirname "$STATE_FILE")" 2>/dev/null

body="$(curl -sf --max-time 10 "$HEALTH_URL" 2>/dev/null)"
if [ -z "$body" ]; then
    alert "$HEALTH_URL unreachable or returned a non-2xx status."
    exit 1
fi

uptime_ticks="$(echo "$body" | grep -oE '"uptime_ticks":[0-9]+' | grep -oE '[0-9]+$')"
if [ -z "$uptime_ticks" ]; then
    alert "$HEALTH_URL responded but 'uptime_ticks' wasn't found in the body -- response shape may have changed. Body: $body"
    exit 1
fi

last_ticks=""
if [ -f "$STATE_FILE" ]; then
    last_ticks="$(cat "$STATE_FILE" 2>/dev/null)"
fi

if [ -n "$last_ticks" ] && [ "$uptime_ticks" -eq "$last_ticks" ] 2>/dev/null; then
    alert "uptime_ticks hasn't moved since the last check ($uptime_ticks both times) -- the process is answering HTTP but its own internal loop may be wedged."
    # Intentionally still record $uptime_ticks below rather than leaving the
    # old state file untouched -- either way the value's identical right
    # now, so this has no effect on this check, but it keeps the file
    # always reflecting "what we last saw," not "what we last saw before
    # things got weird." A stuck process will keep re-alerting on every
    # subsequent run for as long as it stays stuck, which is the correct
    # behavior for an unresolved incident under a simple cron-based check
    # with no separate alert-dedup state.
    echo "$uptime_ticks" > "$STATE_FILE"
    exit 1
fi

echo "$uptime_ticks" > "$STATE_FILE"
echo "[health_check] OK -- uptime_ticks=$uptime_ticks"
