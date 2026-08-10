#!/usr/bin/env bash
# tests/riscv_kernel_copy_rediff_check.sh — the kernel/simi_riscv.{c,h}
# byte-identity tripwire (plan doc §10.184 gap #1).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/simi_riscv.c is a direct, unmodified copy of
# tools/simi/simi_riscv.c below its (kernel-copy-specific) header — that
# byte-identity is the contract that makes the rv64_exec real-execution
# verification of the host copy (run_riscv_tests.sh, and the real RV64
# kernel boot smoke) meaningful for the kernel copy too: same encoder,
# same bugs or lack thereof. Before this check, identity was asserted
# once at commit time and never looked at again — a future edit to
# tools/simi/simi_riscv.c that forgot to re-derive the kernel copy would
# silently diverge the two, and the simi_riscv_kernel_check.sh
# compile/undefined-symbol gate would still pass (it compiles whatever
# kernel/simi_riscv.c is). This guard re-runs the re-diff on every push,
# so a divergence fails CI. Mirror of tests/arm_kernel_copy_rediff_check.sh
# (§10.182) — the ARM pair proved the technique; the §10.184 matrix
# flagged RV64 as the last pair without it.
#
# What it compares: the BODY of each pair — every line from the
# `#include "simi_riscv.h"` marker (.c) / `#ifndef SIMI_RISCV_H` marker
# (.h) to EOF. The headers above those markers are kernel-copy-specific
# prose and are allowed to differ. The marker-anchored extraction (rather
# than hardcoded line numbers) survives header growth.
#
# Toolchain-free: pure text diffing, so it runs in the main CI job via
# tests/run_checks.sh (the `tests/*_check.sh` glob) — no RISC-V toolchain
# required.
#
# Exit: 0 pass, 1 fail (bodies diverged), 2 abort (file or marker missing).
# Optional positional args (paths) let the teeth smoke point the check at
# throwaway copies: <host .c> <kernel .c> <host .h> <kernel .h>.
set -u
cd "$(dirname "$0")/.."   # repo root

HOST_C="${1:-tools/simi/simi_riscv.c}"
KERNEL_C="${2:-kernel/simi_riscv.c}"
HOST_H="${3:-tools/simi/simi_riscv.h}"
KERNEL_H="${4:-kernel/simi_riscv.h}"

# body_c <file> — the lines from the `#include "simi_riscv.h"` marker to EOF.
body_c() { awk '/^#include "simi_riscv.h"$/{f=1} f' "$1"; }
# body_h <file> — the lines from the `#ifndef SIMI_RISCV_H` marker to EOF.
body_h() { awk '/^#ifndef SIMI_RISCV_H$/{f=1} f' "$1"; }

fails=0

for pair in "c:$HOST_C:$KERNEL_C" "h:$HOST_H:$KERNEL_H"; do
    kind="${pair%%:*}"
    rest="${pair#*:}"
    host="${rest%%:*}"
    kernel="${rest#*:}"

    if [ ! -f "$host" ]; then
        echo "ABORT $kind: host file '$host' not found" >&2
        exit 2
    fi
    if [ ! -f "$kernel" ]; then
        echo "ABORT $kind: kernel copy '$kernel' not found" >&2
        exit 2
    fi

    # Marker must exist, or the extraction would be empty on BOTH sides and
    # the diff would falsely pass. An empty body is an abort, not a pass.
    if [ "$kind" = "c" ]; then
        hb=$(body_c "$host")
        kb=$(body_c "$kernel")
    else
        hb=$(body_h "$host")
        kb=$(body_h "$kernel")
    fi
    if [ -z "$hb" ]; then
        echo "ABORT $kind: no '$([ "$kind" = "c" ] && echo '#include "simi_riscv.h"' || echo '#ifndef SIMI_RISCV_H')' marker in '$host'" >&2
        exit 2
    fi
    if [ -z "$kb" ]; then
        echo "ABORT $kind: no marker in '$kernel'" >&2
        exit 2
    fi

    if [ "$hb" = "$kb" ]; then
        echo "OK    $kind: kernel copy body byte-identical to the host copy"
    else
        echo "FAIL  $kind: kernel copy body DIVERGED from the host copy"
        echo "      (re-derive: <new-header> + body of '$host' from its #include/#ifndef marker down)"
        diff <(printf '%s\n' "$hb") <(printf '%s\n' "$kb") | head -10 | sed 's/^/      /'
        fails=$((fails + 1))
    fi
done

echo "riscv_kernel_copy_rediff_check: done, $fails violations"
[ "$fails" -eq 0 ]
