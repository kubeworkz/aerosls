#!/usr/bin/env bash
# tools/go_thread_gate_counts_census.sh — regenerate TG-0's COUNTS census
# (tools/go_thread_gate_census.txt) from the committed fixture.
#
# tools/go_thread_gate_census.sh records the ARGUMENT shapes from a filtered
# trace; this script records the measured syscall SET and counts from a full
# one, then applies the classification the gate doc states: which calls are
# required by the runtime, required by the gate, optional/tolerated, or the
# bootstrap execve. The classification is the script's policy table; a
# measured syscall outside it, or a classified number that never appears, is
# a named failure rather than a silently edited artifact.
#
# Usage:
#   GO=/path/to/go1.25.1/bin/go \
#   GO_TOOLCHAIN_ARCHIVE=/path/to/go1.25.1.linux-amd64.tar.gz \
#   bash tools/go_thread_gate_counts_census.sh
#   OUT=/tmp/counts.txt bash tools/go_thread_gate_counts_census.sh
#
# Requirements: bash, go 1.25.1, the pinned Go tarball, strace, sha256sum,
# cmp, awk, sort, standard coreutils.
# Exit: 0 on success, 2 if a prerequisite is missing, 1 if the run or its
# measured set disagrees with the pinned fixture or the policy.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GO="${GO:-go}"
STRACE="${STRACE:-strace}"
OUT="${OUT:-tools/go_thread_gate_census.txt}"
SRC="tools/go_thread_gate"
readonly TOOLCHAIN_SHA256=7716a0d940a0f6ae8e1f3b3f4f36299dc53e31b16840dbd171254312c41ca12e

for tool in "$GO" "$STRACE" sha256sum cmp mktemp awk grep sort sed; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "ABORT: required tool not found: $tool" >&2
        exit 2
    fi
done

version="$("$GO" version)"
case "$version" in
    "go version go1.25.1 "*) ;;
    *)
        echo "ABORT: the census is pinned to go1.25.1; got: $version" >&2
        exit 2
        ;;
esac

TOOLCHAIN_ARCHIVE="${GO_TOOLCHAIN_ARCHIVE:-}"
if [ -z "$TOOLCHAIN_ARCHIVE" ]; then
    echo "ABORT: set GO_TOOLCHAIN_ARCHIVE to the pinned go1.25.1.linux-amd64.tar.gz" >&2
    exit 2
fi
if [ ! -f "$TOOLCHAIN_ARCHIVE" ]; then
    echo "ABORT: toolchain archive not found: $TOOLCHAIN_ARCHIVE" >&2
    exit 2
fi
archive_sha="$(sha256sum "$TOOLCHAIN_ARCHIVE" | awk '{print $1}')"
if [ "$archive_sha" != "$TOOLCHAIN_SHA256" ]; then
    echo "ABORT: toolchain archive sha256 is $archive_sha; expected $TOOLCHAIN_SHA256" >&2
    exit 2
fi

tmp="$(mktemp -d "${TMPDIR:-/tmp}/go-thread-gate-counts.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

source_sha="$(cd "$SRC" && sha256sum main.go go.mod | awk '{print $2"="$1}' | paste -sd ' ' -)"

( cd "$SRC" && \
  CGO_ENABLED=0 GOOS=linux GOARCH=amd64 "$GO" build \
      -buildmode=exe -trimpath -buildvcs=false \
      -o "$tmp/go_thread_gate" . )

binary_sha="$(sha256sum "$tmp/go_thread_gate" | awk '{print $1}')"
head -c 65536 /dev/zero > "$tmp/in.bin"

# The fixture's futex handshake can lose the race (EAGAIN is observable);
# retry a few times so the census records a completed wait=0/wake=1 run.
attempt=1
while [ "$attempt" -le 10 ]; do
    rm -f "$tmp/out.bin" "$tmp/full.trace" "$tmp/stdout" "$tmp/stderr"
    # D7's gate environment: no GC and no asynchronous preemption, so the
    # trace does not pick up Go's SIGURG machinery (tgkill/rt_sigreturn)
    # and a scheduling accident cannot add rows to the measured set.
    if ! ( cd "$tmp" && \
        GOGC=off GODEBUG=asyncpreemptoff=1 \
        "$STRACE" -f -qq -v -s 256 -o "$tmp/full.trace" -- \
        ./go_thread_gate in.bin out.bin \
        >"$tmp/stdout" 2>"$tmp/stderr" ); then
        echo "ABORT: the fixture or strace failed on attempt $attempt" >&2
        cat "$tmp/stderr" >&2
        exit 1
    fi
    if grep -q '^futex_wait_ret=0 futex_wait_errno=0 futex_wake_ret=1 futex_wake_errno=0$' "$tmp/stdout"; then
        break
    fi
    attempt=$((attempt + 1))
done

if ! grep -q '^futex_wait_ret=0 futex_wait_errno=0 futex_wake_ret=1 futex_wake_errno=0$' "$tmp/stdout"; then
    echo "ABORT: the fixture never produced wait=0/wake=1 in $attempt attempts" >&2
    cat "$tmp/stdout" >&2
    exit 1
fi
grep -q '^main_tid=[0-9][0-9]* worker_tid=[0-9][0-9]*$' "$tmp/stdout" || {
    echo "ABORT: the fixture did not print two TIDs" >&2; cat "$tmp/stdout" >&2; exit 1;
}
grep -q '^copy_bytes=65536$' "$tmp/stdout" || {
    echo "ABORT: the fixture did not copy 65536 bytes" >&2; cat "$tmp/stdout" >&2; exit 1;
}
cmp "$tmp/in.bin" "$tmp/out.bin" || {
    echo "ABORT: the copied bytes differ from the input" >&2; exit 1;
}

# Count syscall ENTRY lines: a call interrupted mid-line still has its entry
# line (`name(... <unfinished ...>`), while the continuation starts with
# `<... name resumed>` and must not be counted again.
sed -E 's/^[0-9]+ //' "$tmp/full.trace" \
    | grep -E '^[a-z0-9_]+\(' \
    | sed -E 's/\(.*//' \
    | sort | uniq -c \
    | awk '{print $2, $1}' | sort > "$tmp/counts.txt"

# The classification contract. Every measured syscall must be here, and
# every number here must be measured — the counts artifact is a partition.
cat > "$tmp/policy.txt" <<'POLICY'
0 read gate
1 write gate
3 close gate
9 mmap runtime
13 rt_sigaction runtime
14 rt_sigprocmask runtime
17 pread64 optional
24 sched_yield optional
28 madvise optional
35 nanosleep optional
56 clone runtime
59 execve bootstrap
60 exit gate
63 uname optional
72 fcntl runtime
74 fsync gate
131 sigaltstack runtime
157 prctl optional
158 arch_prctl runtime
186 gettid gate
202 futex runtime
204 sched_getaffinity optional
231 exit_group runtime
233 epoll_ctl runtime
257 openat runtime
281 epoll_pwait runtime
290 eventfd2 runtime
291 epoll_create1 runtime
302 prlimit64 optional
326 copy_file_range optional
POLICY

awk 'NR > 1 && $1 <= prev {
         printf "ABORT: policy numbers are not strictly increasing at %s\n", $1 > "/dev/stderr"; bad = 1
     }
     { prev = $1 }
     END { exit bad }' "$tmp/policy.txt"

awk 'NR == FNR { known[$2] = 1; next }
     !($1 in known) {
         printf "ABORT: measured syscall is not classified: %s\n", $1 > "/dev/stderr"; bad = 1
     }
     END { exit bad }' "$tmp/policy.txt" "$tmp/counts.txt"

: > "$tmp/rows.txt"
while read -r num name cat; do
    count="$(awk -v n="$name" '$1 == n { print $2 }' "$tmp/counts.txt")"
    if [ -z "$count" ]; then
        echo "ABORT: classified syscall did not appear: $num $name ($cat)" >&2
        exit 1
    fi
    printf '%s %s %s\n' "$num" "$name" "$count" >> "$tmp/rows.txt"
done < "$tmp/policy.txt"

category_list() {
    awk -v c="$1" '$3 == c { printf "%s %s ", $1, $2 }' "$tmp/policy.txt" | sed 's/ $//'
}

tids="$(sed -n 's/^main_tid=\([0-9][0-9]*\) worker_tid=\([0-9][0-9]*\)$/main_tid=\1 worker_tid=\2/p' "$tmp/stdout")"
read -r wait_ret wake_ret <<< "$(awk -F'[= ]' '/^futex_wait_ret=/ { printf "%s %s", $2, $6 }' "$tmp/stdout")"

if grep -qE '^(clock_gettime|gettimeofday) ' "$tmp/counts.txt"; then
    vdso_note="visible clock call in this trace (no vDSO assumed)"
else
    vdso_note="none (host has a vDSO; this gate made no visible clock/gettimeofday call)"
fi

{
    printf '# AeroSLS E7 Go thread-gate syscall census\n'
    printf '# candidate: go_thread_gate\n'
    printf '# generated-by: tools/go_thread_gate_counts_census.sh\n'
    printf '# provenance: docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md (TG-0)\n'
    printf '# args: tools/go_thread_gate_args_census.txt (TG-0b argument shapes)\n'
    printf '# source: %s\n' "$SRC"
    printf '# source-sha256: %s\n' "$source_sha"
    printf '# binary: <ephemeral build output>\n'
    printf '# build: cd %s && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -buildmode=exe -trimpath -buildvcs=false -o <binary> .\n' "$SRC"
    printf '# toolchain: %s\n' "${version#go version }"
    printf '# toolchain-sha256: %s\n' "$TOOLCHAIN_SHA256"
    printf '# binary-sha256: %s\n' "$binary_sha"
    printf '# target: linux/amd64\n'
    printf '# host: %s\n' "$(uname -srm)"
    printf '# date: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '# method: strace -f full trace, entry-counted\n'
    printf '# env: GOGC=off GODEBUG=asyncpreemptoff=1 (D7 gate environment)\n'
    printf '# vdso-backstopped: %s\n' "$vdso_note"
    printf '# workload: 65536-byte file copy, byte-identical input/output\n'
    printf '# threads: %s; futex_wait=%s futex_wake=%s\n' "$tids" "$wait_ret" "$wake_ret"
    printf '# columns: <syscall-number> <name> <count>\n'
    printf '# required-by-runtime (ENOSYS is fatal or corrupts the run): %s\n' "$(category_list runtime)"
    printf '# required-by-gate (the gate itself cannot complete without it): %s\n' "$(category_list gate)"
    printf '# optional/tolerated under forced ENOSYS: %s\n' "$(category_list optional)"
    printf '# bootstrap-only: %s\n' "$(category_list bootstrap)"
    printf '# note: counts are one run and scheduling-sensitive; the row SET is the measured surface\n'
    cat "$tmp/rows.txt"
} > "$OUT"

echo "wrote $OUT"
echo "  rows=$(wc -l < "$tmp/rows.txt" | tr -d ' ') categories=$(awk 'END { print NR }' "$tmp/policy.txt")"
