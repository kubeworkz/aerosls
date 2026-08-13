#!/usr/bin/env bash
#
# code_buffer_budget_check.sh — asserts that the two translation-cache code
# buffers linked into the kernel stay within their declared budgets, and
# remain distinct.
#
# ─── Why this is a build gate and not a code review note ───────────────────
# The claim-verification doc (§1) carries three rows about the code
# buffers:
#
#   sls_code_buffer  — 32 MiB declared, 32.00 MiB in .bss
#   codebuf_storage  — 4 MiB declared, 4.00 MiB in .bss (qemu_sls_codebuf)
#   the two are DIFFERENT buffers — separate symbols, no aliasing
#
# Those rows used to be reproduced by grepping the #defines — which checks
# intent, not the artifact. The far side of the boundary is the LINKED
# image (the same argument no_hosted_link and no_tls_relocations make): a
# declaration that changed without the row being updated, an array
# collapsed to a pointer (linked size 8 instead of 32 MiB), a buffer
# renamed out of existence, or two buffers merged into one would all sail
# past a source grep while silently changing the footprint the kernel is
# sized for — and the runtime symptom (a codegen buffer that is no longer
# the size every allocation is computed against) is exactly the kind of
# fault nobody recognises on the first sighting.
#
# ─── What is checked, and why it is RELATIONAL ─────────────────────────────
# The declared sizes are READ FROM THE SOURCES, never hardcoded here —
# stack_frame_budget's rule: if someone changes the declaration, this
# check changes with it, so a coordinated change (define + row update)
# passes and an uncoordinated one fails. From the linked binary
# (nm -S), the guard asserts:
#
#   1. sls_code_buffer links at exactly SLS_CODE_BUFFER_SIZE  — never above
#      its budget. A static array can only shrink by collapsing to a
#      pointer; a define bump shows up here first.
#   2. codebuf_storage links at exactly QEMU_TCACHE_CODEBUF_SIZE.
#   3. the two buffers' address ranges do not overlap — the "different
#      buffers" row, machine-checked instead of asserted in prose.
#
# Usage:
#   tests/code_buffer_budget_check.sh                 # uses my_sls_kernel.bin
#   tests/code_buffer_budget_check.sh path/to/kernel
#
# Exit: 0 pass, 1 fail, 2 abort (prerequisite missing: no binary, no
# nm, or the declarations could not be read).
set -u
cd "$(dirname "$0")/.."   # repo root, so the source paths resolve

KERNEL="${1:-my_sls_kernel.bin}"

echo "code_buffer_budget_check"
echo "========================"

[ -f "$KERNEL" ] || { echo "ABORT: $KERNEL not found -- run 'make' first" >&2; exit 2; }
command -v nm >/dev/null || { echo "ABORT: nm not found (binutils)" >&2; exit 2; }

fails=0

# ─── 1. the declared budgets, read from the sources ────────────────────────
# Strip the /* comment */, the #define prefix, and U suffixes so the value
# is a plain arithmetic expression bash can evaluate. A missing macro (the
# declaration was renamed away) is a prerequisite failure, not a pass.
decl_code="$(grep -m1 '^#define SLS_CODE_BUFFER_SIZE' ../qemu/sls/sls-runtime.c \
             | sed 's|/\*.*\*/||; s/^#define SLS_CODE_BUFFER_SIZE//; s/[Uu]//g')"
decl_codebuf="$(grep -m1 '^#define QEMU_TCACHE_CODEBUF_SIZE' kernel/qemu_sls_tcache.h \
                | sed 's|/\*.*\*/||; s/^#define QEMU_TCACHE_CODEBUF_SIZE//; s/[Uu]//g')"

BUDGET_CODE="$(( decl_code ))"
BUDGET_CODEBUF="$(( decl_codebuf ))"

if [ "$BUDGET_CODE" -le 0 ] || [ "$BUDGET_CODEBUF" -le 0 ]; then
    echo "ABORT: could not read the declared budgets (SLS_CODE_BUFFER_SIZE /" >&2
    echo "       QEMU_TCACHE_CODEBUF_SIZE). The declarations moved or were" >&2
    echo "       renamed -- update this guard with the new location." >&2
    exit 2
fi

printf "declared : sls_code_buffer = %d bytes, codebuf_storage = %d bytes\n" \
       "$BUDGET_CODE" "$BUDGET_CODEBUF"

# ─── 2. the linked allocations must stay AT their declared budgets ─────────
SZ_CODE="$(nm -S "$KERNEL" 2>/dev/null | awk '$4=="sls_code_buffer" {print $2}')"
SZ_STORAGE="$(nm -S "$KERNEL" 2>/dev/null | awk '$4=="codebuf_storage" {print $2}')"

if [ -z "$SZ_CODE" ]; then
    fails=$((fails + 1))
    echo "FAIL: sls_code_buffer not found in $KERNEL -- the buffer symbol is gone"
    echo "      (renamed? collapsed to a pointer? the claim row is stale)"
else
    linked_code=$((0x$SZ_CODE))
    printf "linked   : sls_code_buffer = %d bytes, codebuf_storage = %d bytes\n" \
           "$linked_code" "$([ -n "$SZ_STORAGE" ] && echo $((0x$SZ_STORAGE)) || echo 0)"
    if [ "$linked_code" -ne "$BUDGET_CODE" ]; then
        fails=$((fails + 1))
        echo "FAIL: sls_code_buffer links at $linked_code bytes -- declared budget"
        echo "      $BUDGET_CODE. It must stay AT its budget (never above); a static"
        echo "      array can only deviate by collapsing or growing the declaration."
    fi
fi

if [ -z "$SZ_STORAGE" ]; then
    fails=$((fails + 1))
    echo "FAIL: codebuf_storage not found in $KERNEL -- the 4 MiB code-gen buffer"
    echo "      symbol is gone (qemu_sls_codebuf is the pointer into it; the buffer"
    echo "      itself must remain the declared 4 MiB .bss allocation)"
else
    linked_storage=$((0x$SZ_STORAGE))
    if [ "$linked_storage" -ne "$BUDGET_CODEBUF" ]; then
        fails=$((fails + 1))
        echo "FAIL: codebuf_storage links at $linked_storage bytes -- declared budget"
        echo "      $BUDGET_CODEBUF. It must stay AT its budget (never above)."
    fi
fi

# ─── 3. the two buffers must be distinct (disjoint address ranges) ─────────
if [ -n "$SZ_CODE" ] && [ -n "$SZ_STORAGE" ]; then
    A=$((0x$(nm -S "$KERNEL" | awk '$4=="sls_code_buffer" {print $1; exit}')))
    B=$((0x$(nm -S "$KERNEL" | awk '$4=="codebuf_storage" {print $1; exit}')))
    A_END=$((A + BUDGET_CODE))
    B_END=$((B + BUDGET_CODEBUF))
    if [ "$A" -lt "$B_END" ] && [ "$B" -lt "$A_END" ]; then
        fails=$((fails + 1))
        echo "FAIL: sls_code_buffer [0x$(printf %x $A), 0x$(printf %x $A_END)) and"
        echo "      codebuf_storage [0x$(printf %x $B), 0x$(printf %x $B_END)) overlap."
        echo "      The claim row 'the two are DIFFERENT buffers' is stale -- a shared"
        echo "      or aliased allocation means one buffer writes into the other."
    fi
fi

echo
if [ "$fails" -ne 0 ]; then
    cat >&2 <<'EOF'
FAILED: a code buffer is not at its declared budget, or the two buffers
are no longer distinct.

  The claim-verification doc (§1) says sls_code_buffer is 32 MiB in .bss,
  codebuf_storage is 4 MiB in .bss, and they are different buffers. Every
  code-gen allocation in the tcache layer is computed against those sizes;
  a silent change makes the runtime footprint and the doc disagree.

  A coordinated change (declaration + doc row together) is legitimate --
  the guard reads the declaration from the source, so it tracks it. What
  fails here is an uncoordinated one: the linked image no longer matches
  the declaration, or the two buffers alias.
EOF
    exit 1
fi
echo "PASS: both code buffers link at their declared budgets and are distinct."
exit 0
