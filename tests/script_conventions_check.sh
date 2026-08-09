#!/usr/bin/env bash
# tests/script_conventions_check.sh — every committed .sh must carry the
# executable bit and a shebang; no committed text file may be stored CRLF.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# Every committed .sh was once 100644, so a Linux checkout got the runners
# non-executable and `make test` failed with Permission denied — the dev
# worktree only worked because drvfs reports every file executable. One
# committed script had no shebang at all, and its blob was CRLF (a shebang
# ending in \r breaks execution with "bad interpreter"). And 128 text blobs
# sat in the repo stored CRLF despite .gitattributes declaring
# `* text=auto eol=lf`. Each of those was a committed violation of the
# declared convention that nothing checked. This guard makes the
# conventions enforced rather than aspirational:
#
#   1. executable bit  — every committed .sh must be 100755 in the index;
#   2. shebang         — every committed .sh must start with `#!`;
#   3. line endings    — no committed text blob may contain a CR byte
#      (binary-marked files are skipped: a PNG or PDF legitimately has CR).
#
# It inspects the INDEX (git ls-files -s / git cat-file -p :<path> /
# git grep --cached), not the working tree — on a Linux checkout the
# `eol=lf` attribute would hide a CRLF blob by converting it on the way
# out, and the executable bit is a property of the committed mode, not the
# checked-out file. In CI the index is the checkout commit; locally it is
# what you have staged, which catches a violation before it is committed.
# Git calls are batched (one ls-files, one grep, per-.sh cat-file only) so
# the guard stays fast even on Windows where each spawn costs ~50 ms.
#
# Runs under tests/run_checks.sh (the `tests/*_check.sh` glob), and
# therefore in CI's "Guard scripts" step and deploy.sh's guard gate. It has
# no prerequisites — it never skips; a guard that inspects nothing protects
# nothing.
#
# Exit: 0 pass, 1 fail (never 2 — there is nothing a prerequisite could be
# missing).
set -u
cd "$(dirname "$0")/.."   # repo root, so the git index paths resolve

fails=0

# 1 + 2. every committed .sh: executable bit + shebang (one ls-files call,
# then one cat-file per .sh — there are only ~25 of them).
while IFS= read -r -d '' entry; do
    path="${entry#*	}"      # after the tab: the path
    mode="${entry%% *}"       # before the first space: 100644 / 100755

    if [ "${path##*.}" = "sh" ]; then
        if [ "$mode" != "100755" ]; then
            echo "FAIL  $path: committed mode $mode, expected 100755 (executable — a Linux checkout cannot run it directly)"
            fails=$((fails + 1))
        fi
        first="$(git cat-file -p ":$path" 2>/dev/null | head -n 1)"
        case "$first" in
            \#!*) ;;
            *)
                echo "FAIL  $path: no shebang (first line is '${first:-<empty>}', expected '#!...')"
                fails=$((fails + 1))
                ;;
        esac
    fi
done < <(git ls-files -s -z)

# 3. line endings — one git grep over the index blobs for a CR byte, then
# check the binary attribute only for the files it hit (usually none).
while IFS= read -r path; do
    if git check-attr --cached binary -- "$path" 2>/dev/null | grep -q "binary: set"; then
        continue
    fi
    echo "FAIL  $path: committed blob is stored CRLF (repo convention: * text=auto eol=lf in .gitattributes)"
    fails=$((fails + 1))
done < <(git grep --cached -l -F "$(printf '\r')" 2>/dev/null || true)

echo "script_conventions_check: done, $fails violations"
[ "$fails" -eq 0 ]
