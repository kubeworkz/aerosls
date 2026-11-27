#!/usr/bin/env bash
# tests/go_thread_gate_census_check.sh — the TG-0 counts artifact is a
# partition, not a list. Every number named by a category line must own a
# row, the row must carry the name the category gives that number, every row
# must sit in exactly one category, and the row numbers must be unique and
# sorted. Without that, the counts can silently cover a syscall twice, drop
# one, rename one under a category's number, or disagree with the doc's
# classification.
#
# This is an internal-consistency check over the committed artifact only: no
# Go, no strace, no boot. tools/go_thread_gate_census.sh is what re-measures.
#
# Run:  bash tests/go_thread_gate_census_check.sh
#       GO_THREAD_GATE_CENSUS=<path> to check another copy.
# Exit: 0 pass, 1 violated, 2 the artifact is missing.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT" || exit 2

ART="${GO_THREAD_GATE_CENSUS:-tools/go_thread_gate_census.txt}"
if [ ! -f "$ART" ]; then
    echo "ABORT: $ART missing — TG-0 counts census not found." >&2
    exit 2
fi

awk '
BEGIN { fails = 0; rows = 0; cats = 0 }

function bad(msg) { print "FAIL: " msg; fails++ }

function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }

# "# <category> (...): <num> <name> <num> <name> ..."
function category(line, name,   rest, n, i, num, tok) {
    rest = line
    sub(/^[^:]*:[ \t]*/, "", rest)
    rest = trim(rest)
    n = split(rest, tok, /[ \t]+/)
    if (rest == "") { bad(name " lists no numbers"); return }
    if (n % 2 != 0) { bad(name " is not <num> <name> pairs: " rest); return }
    for (i = 1; i <= n; i += 2) {
        if (tok[i] !~ /^[0-9]+$/) {
            bad(name " has a non-numeric number: " tok[i])
            continue
        }
        num = tok[i] + 0
        members[num]++
        where[num] = where[num] " " name
        listed[num] = tok[i + 1]
        if (!(num in cat_seen)) { cat_seen[num] = 1; cats++ }
    }
}

function row(line,   f, n, num) {
    n = split(line, f, /[ \t]+/)
    num = f[1] + 0
    rows++
    order[rows] = num
    if (num in rowname) bad("row " num " appears more than once (" rowname[num] " and " f[2] ")")
    rowname[num] = f[2]
}

/^# required-by-runtime/ { category($0, "required-by-runtime"); next }
/^# required-by-gate/    { category($0, "required-by-gate"); next }
/^# optional\/tolerated/ { category($0, "optional/tolerated"); next }
/^# bootstrap-only/      { category($0, "bootstrap-only"); next }

/^[0-9]+[ \t]+[A-Za-z0-9_]+[ \t]+[0-9]+[ \t]*$/ { row($0); next }

END {
    for (i = 2; i <= rows; i++)
        if (order[i] < order[i - 1])
            bad("row " order[i] " is out of order after " order[i - 1])

    for (i = 1; i <= rows; i++) {
        n = order[i]
        if (!(n in members))
            bad("row " n " (" rowname[n] ") is in no category")
        else if (members[n] > 1)
            bad("row " n " (" rowname[n] ") is in " members[n] " categories:" where[n])
    }

    for (n in members) {
        if (!(n in rowname)) {
            bad("category number " n " (" listed[n] ") has no row")
        } else if (listed[n] != rowname[n]) {
            bad("category " trim(where[n]) " names number " n " as " listed[n] " but the row says " rowname[n])
        }
    }

    if (fails == 0) {
        printf "ok:   %d category numbers, each with a matching row and name\n", cats
        printf "ok:   %d rows, each in exactly one category\n", rows
        printf "ok:   row numbers are unique and sorted\n"
    }
    printf "go_thread_gate_census_check: %d rows, %d failed\n", rows, fails
    exit (fails ? 1 : 0)
}
' "$ART"
