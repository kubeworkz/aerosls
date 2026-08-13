#!/usr/bin/env bash
# tests/code_buffer_budget_smoke.sh — proves the code-buffer-budget
# guard's teeth bite, by synthesizing objects whose code buffers violate
# the declared budgets and asserting the guard fails on them.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# tests/code_buffer_budget_check.sh asserts the linked sls_code_buffer and
# codebuf_storage sit exactly at their declared sizes (read from the
# sources) and do not overlap. Its teeth were proven by hand on one build.
# Nothing made it fail automatically, so a regression that made the guard
# blind -- a wrong nm column, a parse that only read the #define and never
# the image, a disjointness check that never fires -- would pass every push
# unnoticed. This smoke proves the size tooth and the presence tooth bite.
#
# ─── How the teeth are synthesized ─────────────────────────────────────────
# The guard reads the DECLARED budgets from the real sources, so the tooth
# only needs to control the linked side. Tooth 1 is an object where
# sls_code_buffer is 16 MiB instead of the declared 32 MiB -- a buffer
# "below" budget is still a mismatch, because a static array can only get
# there by collapsing or by a declaration change the image no longer
# matches. Tooth 2 is an object missing codebuf_storage entirely -- the
# 4 MiB buffer gone while qemu_sls_codebuf (the pointer) remains. Tooth 3
# is the disjointness tooth: an aliased symbol (.set) pointing 16 MiB INTO
# sls_code_buffer with a .size directive, so BOTH sizes are correct and
# only the overlap check can catch it (ld refuses overlapping output
# sections, so the alias is the only honest way to synthesize the
# merge/alias failure the "different buffers" row guards against).
#
# The objects are plain gcc -c outputs in a mktemp dir outside the tree
# (16 MiB + 4 MiB of .bss in a throwaway object is harmless). The trap
# removes the dir on any exit.
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that set.
# This script synthesizes objects on purpose -- it must never run in a
# deploy gate. Run it (and the other build-host smokes) via
# tests/run_guard_smokes.sh after `make`.
#
# Exit: 0 if every tooth bit, 1 otherwise, 2 if gcc is missing.
set -u
cd "$(dirname "$0")/.."   # repo root
guard=tests/code_buffer_budget_check.sh
fails=0

command -v gcc >/dev/null || { echo "ABORT: gcc not found" >&2; exit 2; }

TD="$(mktemp -d)"
trap 'rm -rf "$TD"' EXIT

# ─── Tooth 1: sls_code_buffer at 16 MiB instead of the declared 32 MiB ─────
cat > "$TD/small.c" <<'EOF'
unsigned char sls_code_buffer[16 * 1024 * 1024];
unsigned char codebuf_storage[4 * 1024 * 1024];
EOF
gcc -c "$TD/small.c" -o "$TD/small.o"

out="$(bash "$guard" "$TD/small.o" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "links at"; then
    echo "TOOTH 1 OK   undersized sls_code_buffer -- guard flagged the size mismatch"
    printf '%s\n' "$out" | grep "links at" | sed 's/^/           /'
else
    echo "TOOTH 1 FAIL undersized sls_code_buffer -- guard rc=$rc, expected 'links at' failure"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# ─── Tooth 2: codebuf_storage missing entirely ─────────────────────────────
cat > "$TD/nostorage.c" <<'EOF'
unsigned char sls_code_buffer[32 * 1024 * 1024];
EOF
gcc -c "$TD/nostorage.c" -o "$TD/nostorage.o"

out="$(bash "$guard" "$TD/nostorage.o" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "codebuf_storage not found"; then
    echo "TOOTH 2 OK   missing codebuf_storage -- guard flagged the absent buffer"
else
    echo "TOOTH 2 FAIL missing codebuf_storage -- guard rc=$rc, expected 'not found'"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# ─── Tooth 3: correct sizes, but the buffers OVERLAP ───────────────────────
cat > "$TD/overlap.S" <<'EOF'
  .section .cb1,"aw",@nobits
  .globl sls_code_buffer
sls_code_buffer:
  .zero 32*1024*1024
  .size sls_code_buffer, 32*1024*1024
  .globl codebuf_storage
  .set codebuf_storage, sls_code_buffer + 16*1024*1024
  .size codebuf_storage, 4*1024*1024
EOF
gcc -c "$TD/overlap.S" -o "$TD/overlap.o"
ld -e 0 -o "$TD/overlap" "$TD/overlap.o" 2>/dev/null

out="$(bash "$guard" "$TD/overlap" 2>&1)"
rc=$?
if [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q "overlap"; then
    echo "TOOTH 3 OK   aliased buffers -- guard flagged the overlap"
else
    echo "TOOTH 3 FAIL aliased buffers -- guard rc=$rc, expected 'overlap' failure"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

# ─── Restore: the real kernel must still pass ──────────────────────────────
out="$(bash "$guard" 2>&1)"
if [ $? -eq 0 ]; then
    echo "RESTORE OK   real kernel buffers at budget -- guard passes"
else
    echo "RESTORE FAIL real kernel rejected:"
    printf '%s\n' "$out" | sed 's/^/           /'
    fails=$((fails + 1))
fi

echo "code_buffer_budget_smoke: done, $fails teeth failed"
[ "$fails" -eq 0 ]
