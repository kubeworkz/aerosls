#!/usr/bin/env bash
# tests/run_checks_background.sh — run tests/run_checks.sh in the background
# and preserve its real exit status.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# A background invocation written as:
#
#     wsl.exe bash -lc '... run_checks.sh ...; echo $? > /tmp/rc'
#
# can have the final `$?` expanded by the OUTER shell before WSL starts. The
# remembered status is then 0 even when run_checks.sh printed a FAIL and its
# own summary counted a failure. The runner was correct; the wrapper lost it.
#
# Capturing the status inside this script avoids every launcher's expansion
# rules. The runner is run once, its status is written to STATUS, and the
# wrapper exits with the same status. A timeout is recorded as timeout(1)'s
# 124, not as success.
#
# Usage:
#   bash tests/run_checks_background.sh [LOG] [STATUS]
#
# Defaults:
#   LOG=/tmp/aerosls-run-checks.log
#   STATUS=/tmp/aerosls-run-checks.rc
#
# Env:
#   GUARD_TIMEOUT=120           per-guard budget passed to run_checks.sh
#   RUN_CHECKS_TIMEOUT=1800     total budget for the whole run (0 disables)
#
# Run under a background process, then read STATUS:
#   bash tests/run_checks_background.sh >/dev/null 2>&1 &
#   ... poll /tmp/aerosls-run-checks.rc ...
#
# Exit: run_checks.sh's status, unchanged (including 124 on total timeout).
set -u
cd "$(dirname "$0")/.." || exit 1

LOG="${1:-/tmp/aerosls-run-checks.log}"
STATUS="${2:-/tmp/aerosls-run-checks.rc}"
TOTAL="${RUN_CHECKS_TIMEOUT:-1800}"
GUARD="${GUARD_TIMEOUT:-120}"

if command -v timeout >/dev/null 2>&1; then
    GUARD_TIMEOUT="$GUARD" timeout -k 30 "$TOTAL" bash tests/run_checks.sh >"$LOG" 2>&1
else
    GUARD_TIMEOUT="$GUARD" bash tests/run_checks.sh >"$LOG" 2>&1
fi
rc=$?

printf '%s\n' "$rc" > "$STATUS"
exit "$rc"
