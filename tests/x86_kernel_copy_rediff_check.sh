#!/usr/bin/env bash
# tests/x86_kernel_copy_rediff_check.sh — the kernel/simi_x86.{c,h}
# byte-identity tripwire (plan doc §10.184 gap #1).
#
# ─── Why this exists ───────────────────────────────────────────────────────
# kernel/simi_x86.c is a direct, unmodified copy of tools/simi/simi_x86.c
# below its (kernel-copy-specific) header — that byte-identity is the
# contract that makes the native-x86 real-execution verification of the
# host copy (run_native_tests.sh) meaningful for the kernel copy too:
# same encoder, same bugs or lack thereof. Before this check, identity
# was asserted once at commit time and never looked at again — a future
# edit to tools/simi/simi_x86.c that forgot to re-derive the kernel copy
# would silently diverge the two, and the simi_x86_kernel_check.sh
# compile/undefined-symbol gate would still pass (it compiles whatever
# kernel/simi_x86.c is). This guard re-runs the re-diff on every push, so
# a divergence fails CI. Mirror of tests/arm_kernel_copy_rediff_check.sh
# (§10.182) — the ARM pair proved the technique; the §10.184 matrix
# flagged x86 as the last pair without it.
#
# What it compares: the BODY of each pair — every line from the
# `#include "simi_x86.h"` marker (.c) / `#ifndef SIMI_X86_H` marker (.h)
# to EOF. The headers above those markers are kernel-copy-specific prose
# and are allowed to differ. The marker-anchored extraction (rather than
# hardcoded line numbers) survives header growth.
#
# Toolchain-free: pure text diffing, so it runs in the main CI job via
# tests/run_checks.sh (the `tests/*_check.sh` glob) — no x86 kernel
# toolchain required (the host gcc IS the x86 kernel's toolchain, but
# this check doesn't even need that).
#
# Exit: 0 pass, 1 fail (bodies diverged), 2 abort (file or marker missing).
# Optional positional args (paths) let the teeth smoke point the check at
# throwaway copies: <host .c> <kernel .c> <host .h> <kernel .h>.
set -u
cd "$(dirname "$0")/.."   # repo root

HOST_C="${1:-tools/simi/simi_x86.c}"
KERNEL_C="${2:-kernel/simi_x86.c}"
HOST_H="${3:-tools/simi/simi_x86.h}"
KERNEL_H="${4:-kernel/simi_x86.h}"

# body_c <file> — the lines from the `#include "simi_x86.h"` marker to EOF.
body_c() { awk '/^#include "simi_x86.h"$/{f=1} f' "$1"; }
# body_h <file> — the lines from the `#ifndef SIMI_X86_H` marker to EOF.
body_h() { awk '/^#ifndef SIMI_X86_H$/{f=1} f' "$1"; }

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
        echo "ABORT $kind: no '$([ "$kind" = "c" ] && echo '#include "simi_x86.h"' || echo '#ifndef SIMI_X86_H')' marker in '$host'" >&2
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

echo "x86_kernel_copy_rediff_check: done, $fails violations"
[ "$fails" -eq 0 ]
