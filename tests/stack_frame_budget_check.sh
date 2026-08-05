#!/usr/bin/env bash
#
# stack_frame_budget_check.sh — asserts that no function's stack frame can
# overflow the bootstrap stack.
#
# ─── The bug this exists to catch ──────────────────────────────────────────
# sls_shell_execute() compiles to a 276,032-byte frame. The bootstrap stack was
# 65,536 bytes. Every shell command -- local or over HTTP -- therefore ran with
# RSP about 208 KiB BELOW stack_bottom, writing into whatever .bss followed it.
# Three captured faults put it at 212,768 / 212,864 / 212,880 bytes past the
# bottom; the offset is constant because it is one fixed frame, not recursion.
#
# It survived for months because the trampled memory -- the top of sls_heap,
# the QEMU-SLS bump arena -- was unused. Overwriting memory nobody reads looks
# exactly like working software. Once TCG was linked in and began allocating
# from that arena, the shell's frame and tcg_init_ctx occupied the same bytes
# and nodes halted on the first TCG initialisation.
#
# ─── Why this compares against the ACTUAL stack size ───────────────────────
# -Wframe-larger-than=N in the Makefile takes a fixed N. A fixed N cannot
# express the property that matters, which is RELATIONAL: frames must fit in
# the stack that exists. Raise the stack and the threshold should follow;
# shrink the stack and it must tighten. A threshold that drifts out of step
# with the stack is a check that has stopped meaning anything while continuing
# to pass.
#
# So the stack size is read from the linked binary (stack_top - stack_bottom),
# never hardcoded here. If someone changes boot.asm, this check changes with it.
#
# ─── Two tiers, because they are two different claims ──────────────────────
# HARD FAIL at FAIL_PCT (default 50%): a frame this large cannot nest even
#   once. Frames do nest -- sls_shell_execute() is reached through http_route()
#   (19,280 bytes) and the receive path above it, and calls subsystems below.
#   At >= 100% it does not fit at all and overflows on every single call,
#   deterministically, which is the bug that produced this file.
#
# ADVISORY at WARN_PCT (default 25%): large, worth shrinking, not yet unsafe.
#   Reported and NOT failed, deliberately. The case it was written for was
#   sls_shell_execute() at 276 KB against a 1 MiB stack -- 26%, safe but only
#   3.8x from disaster, and not something to let drift quietly upward.
#
#   That frame is now 10,224 bytes and nothing in the tree is above the
#   advisory line. No current figure is quoted here on purpose: the run prints
#   the live worst frame every time, and a number pasted into a comment is how
#   a fixed problem gets re-reported as an open one months later.
#
# The tiers are separate so the hard gate keeps its meaning. A check that fails
# on a clean checkout gets suppressed, and a suppressed check protects nothing;
# the failure mode this file exists to prevent is exactly a guard that everyone
# has learned to ignore.
#
# Usage:
#   tests/stack_frame_budget_check.sh                 # uses my_sls_kernel.bin
#   tests/stack_frame_budget_check.sh path/to/kernel
#
set -u

KERNEL="${1:-my_sls_kernel.bin}"
WARN_PCT="${WARN_PCT:-25}"
FAIL_PCT="${FAIL_PCT:-50}"

echo "stack_frame_budget_check"
echo "========================"

[ -f "$KERNEL" ] || { echo "ABORT: $KERNEL not found -- run 'make' first" >&2; exit 2; }
command -v nm >/dev/null || { echo "ABORT: nm not found (binutils)" >&2; exit 2; }

# ─── 0b. the binary must be built FROM the sources being judged ────────────
# This check derives the stack size from $KERNEL and the frame sizes from the
# .c files. If the binary is older than the sources, those two halves describe
# different programs and the verdict is meaningless -- which is not theoretical:
# this fired against a binary two weeks older than arch/x86/boot.asm, so it was
# measuring current frames against a 64 KiB stack that had already been raised
# to 1 MiB. It reported a confident FAILED. A stale-binary run must abort, not
# produce a number; the same mtime rule guards deploy.sh for the same reason.
NEWER="$(find . -name '*.c' -o -name '*.h' -o -name '*.asm' 2>/dev/null \
         | while read -r f; do [ "$f" -nt "$KERNEL" ] && echo "$f"; done | head -5)"
if [ -n "$NEWER" ]; then
    echo "ABORT: $KERNEL is older than the sources this check reads." >&2
    echo "       Newer than the binary (first few):" >&2
    echo "$NEWER" | sed 's/^/         /' >&2
    echo "       Run 'make' first. Judging current frames against a stale" >&2
    echo "       binary's stack size gives a confident answer about a program" >&2
    echo "       that was never built." >&2
    exit 2
fi

# ─── 1. the stack size, from the binary rather than from a constant here ───
SB="$(nm "$KERNEL" | awk '$3=="stack_bottom"{print $1}')"
ST="$(nm "$KERNEL" | awk '$3=="stack_top"{print $1}')"
if [ -z "$SB" ] || [ -z "$ST" ]; then
    echo "ABORT: stack_bottom/stack_top not found in $KERNEL." >&2
    echo "       They are 'global' in arch/x86/boot.asm. Without them this" >&2
    echo "       check has no budget to compare against, and passing anyway" >&2
    echo "       would be worse than not running at all." >&2
    exit 2
fi
STACK=$(( 0x$ST - 0x$SB ))
BUDGET=$(( STACK * WARN_PCT / 100 ))
FAIL_AT=$(( STACK * FAIL_PCT / 100 ))
[ "$STACK" -gt 0 ] || { echo "ABORT: computed stack size $STACK is not positive" >&2; exit 2; }

printf "stack    : %d bytes (0x%s .. 0x%s)\n" "$STACK" "$SB" "$ST"
printf "advisory : %d bytes per frame (%s%%)\n" "$BUDGET" "$WARN_PCT"
printf "hard fail: %d bytes per frame (%s%%)\n\n" "$FAIL_AT" "$FAIL_PCT"

# ─── 2. every frame, from a real compile ───────────────────────────────────
FLAGS="-ffreestanding -O2 -Wall -mcmodel=small -mno-red-zone -mno-sse -mno-sse2 \
-mno-mmx -fno-pie -fno-pic -fno-tree-vectorize -I. -Ikernel -Iarch/x86 -Inet \
-Wframe-larger-than=$BUDGET"

command -v gcc >/dev/null || { echo "ABORT: gcc not found" >&2; exit 2; }

over=0
scanned=0
tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

for f in kernel/*.c net/*.c user/*.c arch/x86/*.c; do
    [ -f "$f" ] || continue
    scanned=$((scanned + 1))
    gcc $FLAGS -c "$f" -o /dev/null 2>&1 | awk -v F="$f" '
        /In function/ { fn=$0; sub(/.*In function ./,"",fn); sub(/.:$/,"",fn) }
        /frame size of/ {
            match($0, /frame size of [0-9]+/)
            printf "%d\t%s\t%s\n", substr($0,RSTART+14,RLENGTH-14), F, fn
        }' >> "$tmp"
done

if [ ! -s "$tmp" ]; then
    if [ "$scanned" -eq 0 ]; then
        echo "ABORT: no sources scanned -- run from the repo root" >&2
        exit 2
    fi
    echo "PASS: $scanned file(s) scanned, every frame within $BUDGET bytes."
    exit 0
fi

sort -rn "$tmp" | while IFS=$'\t' read -r sz file fn; do
    pct=$(( sz * 100 / STACK ))
    if   [ "$sz" -ge "$STACK" ];  then tag="OVERFLOWS"
    elif [ "$sz" -ge "$FAIL_AT" ]; then tag="FAIL     "
    else                               tag="advisory "
    fi
    printf "%s %9d bytes (%3d%% of stack)  %s  %s\n" "$tag" "$sz" "$pct" "$file" "$fn"
done

over=$(wc -l < "$tmp")
worst=$(sort -rn "$tmp" | head -1 | cut -f1)
worst_pct=$(( worst * 100 / STACK ))
echo
echo "$scanned file(s) scanned, $over frame(s) above the advisory line,"
echo "worst $worst bytes = ${worst_pct}% of the stack."

if [ "$worst" -ge "$STACK" ]; then
    cat >&2 <<EOF

FAILED: the largest frame ($worst bytes) DOES NOT FIT in the stack ($STACK bytes).
        Not a warning. One call overflows, deterministically, every time, and
        corrupts whatever .bss follows stack_bottom. This is the exact bug the
        file was written for -- sls_shell_execute() at 276,032 bytes against a
        65,536-byte stack, silent for months because the memory it trampled
        happened to be unused.
EOF
    exit 1
fi

if [ "$worst" -ge "$FAIL_AT" ]; then
    cat >&2 <<EOF

FAILED: the largest frame is ${worst_pct}% of the stack (limit ${FAIL_PCT}%).
        It fits alone but cannot nest. sls_shell_execute() is reached through
        http_route() and the receive path above it, and calls subsystems below,
        so "fits alone" is not the condition that matters.
EOF
    exit 1
fi

echo
echo "PASS: every frame fits with nesting room (worst ${worst_pct}%, hard limit ${FAIL_PCT}%)."
echo "      Frames listed as 'advisory' are large and worth shrinking, but are"
echo "      not failures. Do not silence them by raising WARN_PCT."
exit 0
