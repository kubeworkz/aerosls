#!/usr/bin/env bash
# tests/arm_kernel_copy_rediff_check.sh — the kernel/simi_arm.{c,h}
# byte-identity tripwire (M3 kernel half, plan doc §10.180/10.182).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/simi_arm.c is a direct, unmodified copy of tools/simi/simi_arm.c
# below its (kernel-copy-specific) header — that byte-identity is the
# contract that makes the qemu-aarch64 real-execution verification of the
# host copy (plan doc §10.178/10.179) meaningful for the kernel copy too:
# same encoder, same bugs or lack thereof. Before this check, identity was
# asserted once at commit time and never looked at again — a future edit
# to tools/simi/simi_arm.c that forgot to re-derive the kernel copy would
# silently diverge the two, and the arm64-guards compile gate would still
# pass (it compiles whatever kernel/simi_arm.c is). This guard re-runs the
# re-diff on every push, so a divergence fails CI.
#
# What it compares: the BODY of each pair — every line from the
# `#include "simi_arm.h"` marker (.c) / `#ifndef SIMI_ARM_H` marker (.h)
# to EOF. The headers above those markers are kernel-copy-specific prose
# and are allowed to differ. The marker-anchored extraction (rather than
# hardcoded line numbers) survives header growth — the kernel .c header
# grew by six lines in §10.181 without breaking the contract.
#
# Toolchain-free: pure text diffing, so it runs in the main CI job via
# tests/run_checks.sh (the `tests/*_check.sh` glob) — no aarch64
# toolchain required.
#
# Exit: 0 pass, 1 fail (bodies diverged), 2 abort (file or marker missing).
# Optional positional args (paths) let the teeth smoke point the check at
# throwaway copies: <host .c> <kernel .c> <host .h> <kernel .h>.
set -u
cd "$(dirname "$0")/.."   # repo root

HOST_C="${1:-tools/simi/simi_arm.c}"
KERNEL_C="${2:-kernel/simi_arm.c}"
HOST_H="${3:-tools/simi/simi_arm.h}"
KERNEL_H="${4:-kernel/simi_arm.h}"

# body_c <file> — the lines from the `#include "simi_arm.h"` marker to EOF.
body_c() { awk '/^#include "simi_arm.h"$/{f=1} f' "$1"; }
# body_h <file> — the lines from the `#ifndef SIMI_ARM_H` marker to EOF.
body_h() { awk '/^#ifndef SIMI_ARM_H$/{f=1} f' "$1"; }

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
        echo "ABORT $kind: no '$([ "$kind" = "c" ] && echo '#include "simi_arm.h"' || echo '#ifndef SIMI_ARM_H')' marker in '$host'" >&2
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

echo "arm_kernel_copy_rediff_check: done, $fails violations"
[ "$fails" -eq 0 ]
