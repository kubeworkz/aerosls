#!/usr/bin/env bash
# tests/e7_gate_all.sh — run the E7 gate's source, live, teeth and selfkill
# arms as one command and keep the evidence.
#
#   make e7-gate
#
# The harness owns the three live profiles. This driver runs the source clauses
# once, then each profile with its own artifact directory, and copies the
# replayable files into evidence/e7_gate_<UTC>/ (the sparse NVMe images are
# excluded). It stops at the first red arm; that arm's harness.log and every
# completed arm's evidence are already in the bundle. manifest.txt records the
# full and short commit, whether the tree was clean or dirty when the run
# started, each arm's exit status and the built ISO hashes.
#
# Env:
#   E7_GATE_EVIDENCE  evidence directory; must not exist (default timestamped)
#   E7_GATE_CHECK     harness path; test seam for e7_gate_all_smoke.sh
#   E7G_*             passed through; E7G_ISO is unset because each arm owns
#                     its dedicated image
#
# Exit: 0 all green, 1 an arm failed, 2 abort.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)" || exit 2
cd "$ROOT" || exit 2

CHECK="${E7_GATE_CHECK:-$ROOT/tests/e7_gate_check.sh}"
[ -f "$CHECK" ] || { echo "ABORT: no E7 harness at $CHECK" >&2; exit 2; }

# A Git worktree created from Windows (the WSL development path this gate is
# normally run from) writes an absolute `C:/...` path into `.git`. Native WSL
# git then refuses it as "not a git repository", so the one-command run would
# record head=unknown even though the checkout is identifiable. Translate that
# one path shape to /mnt/<drive>/...; a normal Linux checkout has a real .git
# directory and is untouched.
if [ -f "$ROOT/.git" ]; then
    _gitdir=$(sed -n 's/^gitdir: //p' "$ROOT/.git" | head -1)
    case "$_gitdir" in
        [A-Za-z]:/*)
            _drive=$(printf '%s' "$_gitdir" | cut -d: -f1 | tr '[:upper:]' '[:lower:]')
            _rest=${_gitdir#*:}
            _gitdir="/mnt/$_drive/${_rest#/}"
            if [ -f "$_gitdir/HEAD" ]; then
                export GIT_DIR="$_gitdir"
                export GIT_WORK_TREE="$ROOT"
            fi
            ;;
    esac
fi

# Capture identity BEFORE the evidence directory or any build artifact exists:
# the manifest describes the source tree the run started from, not the files
# the run itself generated. A clean CI checkout must read clean here even
# though the run later writes ISO/sidecar/evidence files.
START_HEAD=$(git rev-parse --short HEAD 2>/dev/null || echo unknown)
START_HEAD_FULL=$(git rev-parse HEAD 2>/dev/null || echo unknown)
START_TREE_STATUS=unknown
START_TREE_CHANGED=unknown
if _changed=$(git status --porcelain 2>/dev/null); then
    _changed=$(printf '%s\n' "$_changed" | sed '/^$/d' | wc -l | tr -d ' ')
    START_TREE_CHANGED="$_changed"
    if [ "$_changed" -eq 0 ]; then
        START_TREE_STATUS=clean
    else
        START_TREE_STATUS=dirty
    fi
fi

RUN_DIR="${E7_GATE_EVIDENCE:-evidence/e7_gate_$(date -u +%Y%m%dT%H%M%SZ)}"
[ ! -e "$RUN_DIR" ] || { echo "ABORT: evidence path already exists: $RUN_DIR" >&2; exit 2; }
mkdir -p "$RUN_DIR" || { echo "ABORT: cannot create $RUN_DIR" >&2; exit 2; }

TMP="/tmp/e7-gate-all.$$"
trap 'rm -rf "$TMP"' EXIT

STARTED="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
MODES=(live teeth selfkill)
declare -A RC=()

iso_for() {
    case "$1" in
        live) printf '%s\n' sls_operating_system_e3_drill.iso ;;
        *)    printf '%s\n' "sls_operating_system_e3_$1.iso" ;;
    esac
}

# Copy regular files, but not the 10 GiB sparse disks the harness uses. FIFOs
# (the serial pipes) are excluded by -type f.
copy_artifacts() {
    local src="$1" dst="$2" f rel
    [ -d "$src" ] || return 0
    while IFS= read -r -d '' f; do
        rel="${f#"$src"/}"
        mkdir -p "$dst/$(dirname "$rel")" || return 1
        cp "$f" "$dst/$rel" || return 1
    done < <(find "$src" -type f ! -name '*.img' ! -name '*.raw' -print0 2>/dev/null)
    return 0
}

write_manifest() {
    local mode iso
    {
        echo "started=$STARTED"
        echo "finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "head=$START_HEAD"
        echo "head_full=$START_HEAD_FULL"
        echo "tree_status=$START_TREE_STATUS"
        echo "tree_changed=$START_TREE_CHANGED"
        echo "source.rc=${RC[source]:-not-run}"
        for mode in "${MODES[@]}"; do
            echo "$mode.rc=${RC[$mode]:-not-run}"
        done
        for mode in "${MODES[@]}"; do
            iso="$(iso_for "$mode")"
            if [ -n "${RC[$mode]:-}" ] && [ -f "$ROOT/$iso" ]; then
                echo "$mode.iso=$iso"
                echo "$mode.iso_sha256=$(sha256sum "$ROOT/$iso" | awk '{print $1}')"
            fi
        done
    } > "$RUN_DIR/manifest.txt"
}

run_mode() {
    local mode="$1" dir="$RUN_DIR/$1" rc
    mkdir -p "$TMP/$mode" "$dir" || return 2
    echo "=== e7-gate: $mode ==="
    env -u E7G_ISO E7G_ARTIFACTS="$TMP/$mode" \
        bash "$CHECK" "$ROOT" --"$mode" 2>&1 | tee "$dir/harness.log"
    rc=$?
    copy_artifacts "$TMP/$mode" "$dir" || rc=1
    RC[$mode]=$rc
    echo "  [$mode] exit $rc; evidence: ${dir#"$ROOT"/}"
    return "$rc"
}

fail() {
    write_manifest
    echo "e7_gate_all: FAIL — $1; evidence in ${RUN_DIR#"$ROOT"/}" >&2
    exit 1
}

echo "e7_gate_all: evidence will be written to ${RUN_DIR#"$ROOT"/}"
echo "=== e7-gate: source clauses ==="
bash "$CHECK" "$ROOT" 2>&1 | tee "$RUN_DIR/source.log"
RC[source]=$?
[ "${RC[source]}" -eq 0 ] || fail "the source clauses are red"

for mode in "${MODES[@]}"; do
    run_mode "$mode" || fail "$mode red"
done

write_manifest || { echo "e7_gate_all: FAIL — could not write $RUN_DIR/manifest.txt" >&2; exit 1; }
echo "e7_gate_all: PASS — source, live, teeth and selfkill green; evidence in ${RUN_DIR#"$ROOT"/}"
