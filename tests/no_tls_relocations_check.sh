#!/usr/bin/env bash
#
# no_tls_relocations_check.sh — asserts that no object linked into the kernel
# contains a thread-local-storage relocation.
#
# ─── Why this is a build gate and not a code review note ───────────────────
# QEMU declares tcg_ctx (and four others) __thread. On x86-64, GCC compiles a
# __thread access in the local-exec model to a segment-relative reference --
# a negative displacement from the thread pointer held in FS. AeroSLS never
# writes MSR_FS_BASE, so FS has base 0 and every such access resolves to a
# linear address just below zero.
#
# The first statement of tcg_init() assigns tcg_ctx, which the compiler emitted
# as:
#
#     movq $tcg_init_ctx, %fs:-8
#
# a WRITE to 0xfffffffffffffff8. #PF, error=0x2, on the first line of TCG
# initialisation. It halted a cluster node on every `qemu bench` invocation.
#
# sls/sls-osdep.h now defines __thread away under SLS_IN_KERNEL. That fix is
# one line in a header that is force-included via -include, which makes it
# exactly the kind of thing a future -I reordering, a new source file compiled
# with different flags, or a rebased QEMU can silently undo. The symptom would
# not be a compile error. It would be a node that halts with a fault address
# nobody recognises, and it took a full session to recognise it the first time.
#
# ─── What is checked, and why it is the RELOCATION ─────────────────────────
# Not "does sls-osdep.h contain the #define" -- that is true whether or not the
# define reaches the translation unit, which is the only thing that matters.
# Not "does the source say __thread" -- QEMU's sources still do, and should.
#
# The relocation is the far side of the boundary: it is what the compiler
# actually emitted, after every include path, macro, and flag had its say. A
# TPOFF/TLS relocation in a linked object means some access WILL go through FS
# at run time, whatever the headers appear to say.
#
# Usage:
#   tests/no_tls_relocations_check.sh                # checks tcg-objs/ + *.x86.o
#   tests/no_tls_relocations_check.sh path/to/dir    # or an explicit directory
#
set -u

SEARCH="${1:-}"
checked=0
bad=0

echo "no_tls_relocations_check"
echo "========================"

if [ -n "$SEARCH" ]; then
    [ -d "$SEARCH" ] || { echo "ABORT: '$SEARCH' is not a directory" >&2; exit 2; }
    mapfile -t OBJS < <(find "$SEARCH" -name '*.o' 2>/dev/null | sort)
else
    mapfile -t OBJS < <({ find tcg-objs -name '*.o' 2>/dev/null
                          find . -maxdepth 3 -name '*.x86.o' 2>/dev/null; } | sort -u)
fi

if [ "${#OBJS[@]}" -eq 0 ]; then
    echo "ABORT: no object files found. Run 'make' first -- this checks build" >&2
    echo "       output, and an empty sweep passing would be the exact kind of" >&2
    echo "       vacuous success this file exists to prevent." >&2
    exit 2
fi

command -v readelf >/dev/null || { echo "ABORT: readelf not found (binutils)" >&2; exit 2; }

for o in "${OBJS[@]}"; do
    checked=$((checked + 1))
    # TPOFF32/TPOFF64 are local-exec; GOTTPOFF is initial-exec; DTPMOD/DTPOFF
    # are general/local-dynamic. Any of them means an FS-relative access.
    hits="$(readelf -r "$o" 2>/dev/null | grep -E 'TPOFF|GOTTPOFF|DTPMOD|DTPOFF' || true)"
    if [ -n "$hits" ]; then
        bad=$((bad + 1))
        echo "FAIL: $o"
        echo "$hits" | sed 's/^/        /'
    fi
done

echo
echo "checked $checked object(s)"
if [ "$bad" -ne 0 ]; then
    cat >&2 <<'EOF'
FAILED: thread-local relocations are present.

  At run time these become FS-relative accesses. FS_BASE is 0 in this kernel,
  so each one reads or writes an address just below zero and takes a #PF with
  error=0x2 (write) or 0x0 (read) at 0xfffffffff...

  Check that -include ../qemu/sls/sls-osdep.h is still on the command line for
  the offending object, and that SLS_IN_KERNEL is still defined for it.
EOF
    exit 1
fi
echo "PASS: no thread-local relocations in any linked object."
exit 0
