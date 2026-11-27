#!/usr/bin/env bash
# tools/go_thread_gate_census.sh — TG-0b: measure the ARGUMENTS of the Go
# thread-gate fixture, not just the syscall counts.
#
# The TG-0 counts artifact (tools/go_thread_gate_census.txt) records the
# exercised set. The thread-group design also depends on the shapes behind
# those counts: the exact clone flags, the private futex operations and their
# timeout forms, the epoll_pwait timeout, the openat dirfd/flags, and the
# rt_sigaction action flags. This script builds the fixture with the pinned
# Go 1.25.1 toolchain and writes one strace run to:
#
#   tools/go_thread_gate_args_census.txt
#
# The run is host-only. It does not boot AeroSLS and does not claim a shim
# syscall works; it is the measured argument contract TG-1 implements.
#
# Usage:
#   GO=/path/to/go1.25.1/bin/go \
#   GO_TOOLCHAIN_ARCHIVE=/path/to/go1.25.1.linux-amd64.tar.gz \
#   bash tools/go_thread_gate_census.sh
#   OUT=/tmp/args.txt bash tools/go_thread_gate_census.sh
#
# Requirements: bash, go 1.25.1, the pinned Go tarball, strace, sha256sum,
# cmp, standard coreutils.
# Exit: 0 on success, 2 if a prerequisite is missing, 1 if the run or its
# measured shape does not match the pinned fixture.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

GO="${GO:-go}"
STRACE="${STRACE:-strace}"
OUT="${OUT:-tools/go_thread_gate_args_census.txt}"
SRC="tools/go_thread_gate"
readonly TOOLCHAIN_SHA256=7716a0d940a0f6ae8e1f3b3f4f36299dc53e31b16840dbd171254312c41ca12e

for tool in "$GO" "$STRACE" sha256sum cmp mktemp; do
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

tmp="$(mktemp -d "${TMPDIR:-/tmp}/go-thread-gate-census.XXXXXX")"
trap 'rm -rf "$tmp"' EXIT

source_sha="$(cd "$SRC" && sha256sum main.go go.mod | awk '{print $2"="$1}' | paste -sd ' ' -)"

( cd "$SRC" && \
  CGO_ENABLED=0 GOOS=linux GOARCH=amd64 "$GO" build \
      -buildmode=exe -trimpath -buildvcs=false \
      -o "$tmp/go_thread_gate" . )

binary_sha="$(sha256sum "$tmp/go_thread_gate" | awk '{print $1}')"
head -c 65536 /dev/zero > "$tmp/in.bin"

# The fixture's futex handshake can lose the race (EAGAIN is observable);
# retry a few times so the census records the intended wait=0/wake=1 shape.
attempt=1
while [ "$attempt" -le 10 ]; do
    rm -f "$tmp/out.bin" "$tmp/args.trace" "$tmp/stdout" "$tmp/stderr"
    if ! ( cd "$tmp" && \
        "$STRACE" -f -qq -v -s 256 -o "$tmp/args.trace" \
        -e trace=clone,futex,epoll_pwait,openat,rt_sigaction -- \
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

selected="$(grep -E ' (clone|futex|epoll_pwait|openat|rt_sigaction)\(' "$tmp/args.trace" | sed -E 's/^[0-9]+ //')"
[ -n "$selected" ] || { echo "ABORT: strace selected no rows" >&2; exit 1; }

clone_count="$(printf '%s\n' "$selected" | grep -c '^clone(' || true)"
futex_count="$(printf '%s\n' "$selected" | grep -c '^futex(' || true)"
epoll_count="$(printf '%s\n' "$selected" | grep -c '^epoll_pwait(' || true)"
openat_count="$(printf '%s\n' "$selected" | grep -c '^openat(' || true)"
sig_count="$(printf '%s\n' "$selected" | grep -c '^rt_sigaction(' || true)"
sig_install_count="$(printf '%s\n' "$selected" | grep -c '^rt_sigaction([^,]*, {sa_handler=' || true)"
sig_query_count="$(printf '%s\n' "$selected" | grep -c '^rt_sigaction(.*NULL, {sa_handler=' || true)"

clone_flags="$(printf '%s\n' "$selected" | sed -n 's/^clone(.*flags=\([^,]*\),.*/\1/p' | sort -u | paste -sd ';' -)"
futex_ops="$(printf '%s\n' "$selected" | sed -n 's/^futex([^,]*, \([^,]*\),.*/\1/p' | sort -u | paste -sd ';' -)"
futex_timeouts="$(printf '%s\n' "$selected" | grep -c '^futex(.*tv_sec=' || true)"
futex_null="$(printf '%s\n' "$selected" | grep -c '^futex(.*NULL' || true)"
epoll_nonzero="$(printf '%s\n' "$selected" | grep '^epoll_pwait(' | grep -vc ', 0, NULL, 0)' || true)"
openat_nonfdcwd="$(printf '%s\n' "$selected" | grep '^openat(' | grep -vc 'AT_FDCWD' || true)"

case "$clone_count" in ''|0) echo "ABORT: no clone rows" >&2; exit 1;; esac
case "$futex_count" in ''|0) echo "ABORT: no futex rows" >&2; exit 1;; esac
case "$epoll_count" in ''|0) echo "ABORT: no epoll_pwait rows" >&2; exit 1;; esac
case "$openat_count" in ''|0) echo "ABORT: no openat rows" >&2; exit 1;; esac
case "$sig_count" in ''|0) echo "ABORT: no rt_sigaction rows" >&2; exit 1;; esac
[ "$epoll_nonzero" = 0 ] || { echo "ABORT: epoll_pwait has a non-zero timeout/sigmask" >&2; exit 1; }
[ "$openat_nonfdcwd" = 0 ] || { echo "ABORT: openat has a non-AT_FDCWD dirfd" >&2; exit 1; }

case "$clone_flags" in
    *CLONE_VM*CLONE_THREAD*CLONE_SETTLS*) ;;
    *) echo "ABORT: clone flags do not contain the measured Go thread shape: $clone_flags" >&2; exit 1;;
esac
case "$clone_flags" in
    *CLONE_CHILD_CLEARTID*) echo "ABORT: clone unexpectedly uses CLONE_CHILD_CLEARTID" >&2; exit 1;;
esac

{
    printf '# AeroSLS E7 Go thread-gate TG-0b argument census\n'
    printf '# candidate: go_thread_gate\n'
    printf '# generated-by: tools/go_thread_gate_census.sh\n'
    printf '# provenance: docs/AeroSLS-E7-Thread-Group-Gate-v0.1.md (TG-0b, I0)\n'
    printf '# source: %s\n' "$SRC"
    printf '# source-sha256: %s\n' "$source_sha"
    printf '# build: cd %s && CGO_ENABLED=0 GOOS=linux GOARCH=amd64 go build -buildmode=exe -trimpath -buildvcs=false -o <binary> .\n' "$SRC"
    printf '# toolchain: %s\n' "$version"
    printf '# toolchain-archive-sha256: %s (verified from %s)\n' \
        "$TOOLCHAIN_SHA256" "$(basename "$TOOLCHAIN_ARCHIVE")"
    printf '# binary-sha256: %s\n' "$binary_sha"
    printf '# target: linux/amd64\n'
    printf '# host: %s\n' "$(uname -a)"
    printf '# date: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    printf '# method: strace -f -qq -v -s 256 -e trace=clone,futex,epoll_pwait,openat,rt_sigaction\n'
    printf '# note: PIDs are stripped; addresses and handler words are one run, not the contract.\n'
    printf '# workload: 65536-byte copy, byte-identical; %s\n' "$(head -1 "$tmp/stdout")"
    printf '# futex: %s\n' "$(grep '^futex_wait_ret=' "$tmp/stdout")"
    printf '# copy: %s\n' "$(grep '^copy_bytes=' "$tmp/stdout")"
    printf '# clone: %s rows; flags: %s\n' "$clone_count" "$clone_flags"
    printf '# futex: %s rows; ops: %s; relative-timeouts: %s; null-timeouts: %s\n' \
        "$futex_count" "$futex_ops" "$futex_timeouts" "$futex_null"
    printf '# epoll_pwait: %s rows; nonzero-timeout-or-sigmask: %s\n' "$epoll_count" "$epoll_nonzero"
    printf '# openat: %s rows; non-AT_FDCWD: %s\n' "$openat_count" "$openat_nonfdcwd"
    printf '# rt_sigaction: %s rows; installs: %s; queries: %s\n' \
        "$sig_count" "$sig_install_count" "$sig_query_count"
    printf '# --- selected trace (one run; argument shape is the measured contract) ---\n'
    printf '%s\n' "$selected"
} > "$OUT"

echo "wrote $OUT"
echo "  clone=$clone_count futex=$futex_count epoll_pwait=$epoll_count openat=$openat_count rt_sigaction=$sig_count"
