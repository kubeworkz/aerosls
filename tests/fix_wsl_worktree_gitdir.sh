#!/usr/bin/env bash
# tests/fix_wsl_worktree_gitdir.sh — rewrite a git worktree's forward .git
# pointer to the RELATIVE form, the one form both Git for Windows and Linux
# git under WSL resolve natively.
#
# ─── The bug this fixes ────────────────────────────────────────────────────
# When a Windows-side git (Freebuff, Git for Windows) creates a worktree, the
# worktree's .git FILE contains an absolute Windows-native path:
#
#     gitdir: C:/Users/.../.git/worktrees/<name>
#
# Linux git under WSL treats "C:/..." as a RELATIVE path and prepends the
# cwd, producing /mnt/c/.../C:/Users/... -> "fatal: not a git repository".
# Every git-based guard then reads EMPTY output and misreports: tls_key_
# containment_check fabricated "used by NOTHING -- the store is not wired
# up", script_conventions_smoke's teeth could not bite, vendor_selfcontained
# claimed tracked files were untracked -- and script_conventions_check
# false-passed while inspecting nothing. The mirror image is just as real:
# a worktree created from WSL gets `gitdir: /mnt/c/...`, which Git for
# Windows cannot resolve.
#
# The RELATIVE form is the intersection of the two dialects. git core
# resolves a relative gitdir: line against the directory containing the .git
# file, identically in both implementations (verified empirically: absolute
# forms only ever work for one git, and a relative REVERSE pointer breaks
# `git worktree list`'s prunable bookkeeping). So this script rewrites only
# the FORWARD pointer -- the worktree's .git file -- to
#
#     gitdir: ../../../.git/worktrees/<name>
#
# and leaves the admin-dir reverse pointer (worktrees/<name>/gitdir) alone.
# That one must stay Windows-native for `git worktree list`/`prune`; the
# asymmetry is what keeps the Windows side fully functional.
#
# ─── Scope and safety ──────────────────────────────────────────────────────
# Only the forward pointer is touched, and only when it is an ABSOLUTE path
# (Windows-drive or POSIX) that the other git cannot resolve. Already-
# relative pointers are left alone, so the script is idempotent. The target
# gitdir is verified to exist and look like a git admin directory BEFORE
# anything is written; afterwards `git -C <worktree> rev-parse --git-dir`
# must succeed, and the original line is restored on failure. It refuses to
# run when the worktree and the admin dir sit on different drives, where no
# relative path exists. The reverse pointer is never touched.
#
# Usage:
#   tests/fix_wsl_worktree_gitdir.sh [path-to-worktree]   # default: cwd
#
# Exit: 0 fixed or already relative, 1 rewrite failed verification,
#       2 not a worktree / bad input / cross-drive.
set -u

WT="${1:-.}"
GITFILE="$WT/.git"

# ─── a worktree has a .git FILE; a main repo has a .git DIRECTORY ─────────
if [ ! -f "$GITFILE" ]; then
    echo "ABORT: $GITFILE is not a file -- $WT is not a git worktree" >&2
    echo "       (a main repository has a .git directory, which needs no fix)." >&2
    exit 2
fi

line="$(sed -n '1s/^gitdir: *//p' "$GITFILE")"
if [ -z "$line" ]; then
    echo "ABORT: $GITFILE does not start with 'gitdir: <path>'." >&2
    exit 2
fi

# ─── already relative? nothing to do (idempotent) ─────────────────────────
case "$line" in
    /*|[A-Za-z]:*) ;;                  # absolute: Windows-drive or POSIX
    *)
        echo "ok:   .git gitdir line is already relative ('$line') -- nothing to do."
        exit 0 ;;
esac

# ─── normalise the target into the CURRENT shell's dialect so it can be
# stat()ed and compared: C:/... -> /mnt/c/... under WSL, /c/... in Git
# Bash; /mnt/c/... -> /c/... in Git Bash. Backslashes are folded too. ─────
norm="$(printf '%s' "$line" | tr '\\' '/')"
case "$norm" in
    [A-Za-z]:*)
        d="$(printf '%s' "$norm" | cut -c1 | tr 'A-Z' 'a-z')"
        norm="${norm#?:}"
        case "$(pwd)" in
            /mnt/*) norm="/mnt/$d/${norm#/}" ;;
            /*)     norm="/$d/${norm#/}" ;;
            *)      echo "ABORT: cannot map Windows path '$line' into this shell's" >&2
                    echo "       filesystem dialect (unrecognised root layout)." >&2
                    exit 2 ;;
        esac
        ;;
    /mnt/*)
        case "$(pwd)" in
            /mnt/*) ;;                              # already WSL-native
            /*)     norm="/$(printf '%s' "$norm" | cut -d/ -f3)/${norm#/mnt/*/}" ;;
            *)      echo "ABORT: cannot map '$line' into this shell's dialect." >&2; exit 2 ;;
        esac
        ;;
    /*) ;;                                          # plain POSIX, already native
esac

[ -d "$norm" ] || {
    echo "ABORT: gitdir '$line' resolves to '$norm', which is not a directory." >&2
    exit 2
}
[ -f "$norm/HEAD" ] || {
    echo "ABORT: '$norm' does not look like a git admin directory (no HEAD)." >&2
    exit 2
}

src="$(cd "$WT" && pwd)" || exit 2

# ─── refuse a relative path that would cross drives ───────────────────────
drive_of() {
    case "$1" in
        /mnt/[A-Za-z]*) printf '%s' "$1" | cut -d/ -f3 ;;
        /[A-Za-z]/*)    printf '%s' "$1" | cut -d/ -f2 ;;
        *)              printf '' ;;
    esac
}
ds="$(drive_of "$src")"; dt="$(drive_of "$norm")"
if [ -n "$ds" ] && [ -n "$dt" ] && [ "$ds" != "$dt" ]; then
    echo "ABORT: worktree ($src) and admin gitdir ($norm) are on different" >&2
    echo "       drives ('$ds' vs '$dt'); no relative pointer exists." >&2
    exit 2
fi

# ─── relative path from src to norm ───────────────────────────────────────
relpath() {
    local from="$1" to="$2"
    local IFS=/ ; local fa=($from) ta=($to)
    local i=0 common=0 j ups="" rest=""
    while [ "$i" -lt "${#fa[@]}" ] && [ "$i" -lt "${#ta[@]}" ] \
          && [ "${fa[$i]}" = "${ta[$i]}" ]; do
        common=$((common + 1)); i=$((i + 1))
    done
    for ((j = common; j < ${#fa[@]}; j++)); do ups="${ups}../"; done
    for ((j = common; j < ${#ta[@]}; j++)); do rest="${rest}${ta[$j]}/"; done
    printf '%s%s\n' "$ups" "${rest%/}"
}
rel="$(relpath "$src" "$norm")"

# ─── rewrite with an in-memory backup, verify, restore on failure ─────────
orig="$(cat "$GITFILE")"
printf 'gitdir: %s\n' "$rel" > "$GITFILE"
if git -C "$WT" rev-parse --git-dir >/dev/null 2>&1; then
    echo "ok:   rewrote $GITFILE to:"
    echo "        gitdir: $rel"
    echo "      was:"
    echo "        $(printf '%s' "$orig" | sed 's/^gitdir: *//')"
    echo "      Reverse pointer (worktrees/<name>/gitdir) left untouched."
else
    printf '%s\n' "$orig" > "$GITFILE"
    echo "FAIL: git cannot resolve the worktree after the rewrite --" >&2
    echo "      restored the original gitdir line." >&2
    exit 1
fi
