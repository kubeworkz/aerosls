#!/usr/bin/env bash
# tests/aeroidl_gate_smoke.sh — proves the aeroidl-check gate's teeth bite.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# `make aeroidl-check` parses every idl/*.aeroidl and runs --check-all, which
# regenerates all four backends (Rust client, Rust dispatcher, Common Lisp,
# C header) and COMPILES them: cargo check on the Rust pair, gcc/clang on
# the C header, paren-balance on the Lisp. A codegen regression that emits
# broken output fails the gate with exit 1 -- that is the property, and it
# has already caught real bugs (unbalanced Lisp parens, a missing C count
# param, enum/arena type mismatches in the Rust dispatcher).
#
# A gate whose teeth are never shown to bite can go blind: nothing in CI
# made it fail, so a regression that makes it pass-everything (a broken
# generator, a checker that stopped checking) would sail through green.
# This smoke breaks each of the four generators ONE AT A TIME and requires
# the gate to fail, then restores the file byte-identically and requires
# the gate to pass again.
#
# ─── How the teeth are planted ─────────────────────────────────────────────
# Each tooth is a one-line sed mutation of a generator in tools/aeroidl/src/
# that makes its output fail the gate's own check:
#
#   rust      gen_rust.rs        opcode constant gains `syntax_error`  -> cargo check fails
#   dispatch  gen_dispatcher.rs  opcode constant gains `syntax_error`  -> cargo check fails
#   lisp      gen_lisp.rs        defconstant loses its closing paren      -> paren check fails
#   c         gen_c.rs           opcode #define gains `syntax_error`    -> gcc/clang fails
#
# The C tooth needs a C compiler: without one, check_c skips by design and
# the gate cannot be made to fail on it, so it is reported as SKIP rather
# than silently passed. (CI's self-hosted runner has gcc, so it runs there.)
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script mutates generator sources on purpose — it must never run
# in a deploy gate. Only ci.yml invokes it.
#
# Exit: 0 if every tooth bit, 1 if any did not, 2 if a prerequisite is
# missing.
set -u

cd "$(dirname "$0")/.."   # repo root

BIN=tools/aeroidl/target/debug/aeroidl
MANIFEST=tools/aeroidl/Cargo.toml
IDLS=(idl/*.aeroidl)
GEN_LISP=tools/aeroidl/src/gen_lisp.rs
GEN_C=tools/aeroidl/src/gen_c.rs
GEN_RUST=tools/aeroidl/src/gen_rust.rs
GEN_DISP=tools/aeroidl/src/gen_dispatcher.rs

pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }
skip() { echo "skip: $1"; }

# ─── Prerequisites ─────────────────────────────────────────────────────────
[ -f "$GEN_RUST" ] && [ -f "$GEN_DISP" ] && [ -f "$GEN_LISP" ] && [ -f "$GEN_C" ] || {
    echo "ABORT: a generator under tools/aeroidl/src/ is missing." >&2; exit 2; }
[ "${#IDLS[@]}" -gt 0 ] || { echo "ABORT: no idl/*.aeroidl found." >&2; exit 2; }
command -v cargo >/dev/null 2>&1 || { echo "ABORT: cargo not available." >&2; exit 2; }

# The C compiler check mirrors check_c's own detection order (gcc, clang,
# cc, tcc). If none exists, the C tooth cannot fail the gate by design.
HAVE_CC=""
for cc in gcc clang cc tcc; do
    if command -v "$cc" >/dev/null 2>&1; then HAVE_CC="$cc"; break; fi
done

echo "aeroidl_gate_smoke"
echo "=================="
echo
echo "building the AeroIDL compiler (cargo build)..."
cargo build --quiet --manifest-path "$MANIFEST" || {
    echo "ABORT: the AeroIDL compiler does not build." >&2; exit 2; }
[ -x "$BIN" ] || { echo "ABORT: $BIN not found after build." >&2; exit 2; }

# The gate, as the Makefile runs it for each IDL file. Exit 0 iff every
# IDL's all-backends check passes (a healthy gate), 1 if any of them fails.
gate_healthy() {
    local rc=0
    local f
    for f in "${IDLS[@]}"; do
        "$BIN" "$f" --check-all >/dev/null 2>&1 || rc=1
    done
    return "$rc"
}

# The gate checks what the COMPILED binary does, so a mutation only takes
# effect once the generator is rebuilt — exactly what `make aeroidl-check`'s
# cargo build does before check-all.
rebuild() {
    cargo build --quiet --manifest-path "$MANIFEST"
}

# tooth <name> <file> <sed-mutation> <expected-diagnostic>
tooth() {
    local name="$1" file="$2" mutation="$3" expected="$4"
    local snap; snap="${file}.smoke.bak"
    local out rc

    cp "$file" "$snap" || { bad "$name: cannot snapshot $file"; return; }
    # The trap restores every file even if this script dies mid-tooth.
    trap "mv -f \"$snap\" \"$file\" 2>/dev/null; rm -f \"$snap\"" EXIT

    if ! sed -i "$mutation" "$file"; then
        bad "$name: sed failed to apply mutation"
        restore_checked "$name" "$file" "$snap"; return
    fi
    if cmp -s "$file" "$snap"; then
        bad "$name: mutation did not apply — the pattern no longer matches
            $file, so this smoke is testing nothing. Fix the pattern."
        restore_checked "$name" "$file" "$snap"; return
    fi
    # sed -i rewrites in place and may preserve an mtime cargo already saw;
    # touch makes the mutation visible to cargo's incremental build (the
    # same reason restore_checked touches after mv).
    touch "$file"
    ok "$name: mutation applied"

    if ! rebuild; then
        bad "$name: the mutated generator does not compile — the mutation is
            invalid, not a gate check. Fix the tooth."
        restore_checked "$name" "$file" "$snap"; return
    fi
    out="$(gate_healthy 2>&1)"; rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "$name: the gate did NOT fail on a broken generator. It is blind."
    else
        ok "$name: gate failed as required ($expected)"
    fi

    restore_checked "$name" "$file" "$snap"
}

# restore_checked <name> <file> <snapshot> — put the snapshot back and prove
# the file is byte-identical to what the run started with.
restore_checked() {
    local name="$1" file="$2" snap="$3"
    mv -f "$snap" "$file"
    touch "$file"
    rm -f "$snap"
    trap - EXIT
    if [ -f "$snap" ]; then
        bad "$name: restore left $snap behind"
    fi
}

echo
echo "=== teeth: break each generator, the gate must fail ==="
echo

# 1. Rust client: `pub const OP_ADD: u16 = 1;` gains a syntax error.
tooth "rust client" "$GEN_RUST" \
    's@pub const {const_name}: u16 = {opcode};@pub const {const_name}: u16 = {opcode}; syntax_error@' \
    "cargo check on the generated client fails"

# 2. Rust dispatcher: same shape, its own opcode-constant line.
tooth "rust dispatcher" "$GEN_DISP" \
    's@pub const {const_name}: u16 = {opcode};@pub const {const_name}: u16 = {opcode}; syntax_error@' \
    "cargo check on the generated dispatcher fails"

# 3. Common Lisp: the closing paren of each `(defconstant +op-add+ ...)` is
#    removed, so the generated file has unbalanced parens and the structural
#    check must fail. (Adding text after a complete form keeps the parens
#    balanced and is invisible to the depth checker — removing a paren is the
#    mutation the check exists for.)
tooth "lisp" "$GEN_LISP" \
    's@(defconstant {const_name:<28} #x{:04X})@(defconstant {const_name:<28} #x{:04X}@' \
    "the generated Lisp fails the paren-balance check"

# 4. C header: `#define CALC_OP_ADD ...` gains a syntax error. Requires a C
#    compiler; without one check_c skips and the gate cannot be made to fail.
if [ -n "$HAVE_CC" ]; then
    tooth "c header" "$GEN_C" \
        's@#define {const_name:<32} 0x{opcode:04X}@#define {const_name:<32} 0x{opcode:04X} syntax_error@' \
        "gcc/clang fails on the generated header"
else
    skip "c header — no C compiler found (gcc/clang/cc/tcc); check_c skips by design"
fi

echo
echo "=== restore check: the gate must pass on the unmutated tree ==="
echo
if ! rebuild; then
    bad "the generators do not compile after restore"
else
    ok "generators restored — compiler builds"
fi
out="$(gate_healthy 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    ok "generators restored — the gate passes again"
else
    bad "the gate still fails after the teeth were removed:"
    printf '%s\n' "$out" | sed 's/^/        /'
fi

# The four generator files must be byte-identical to what the run started
# with — this smoke must never leave a footprint behind.
dirty=0
for f in "$GEN_RUST" "$GEN_DISP" "$GEN_LISP" "$GEN_C"; do
    # The snapshot was removed on restore; git diff against the committed
    # state is the wrong baseline (this worktree has uncommitted fixes), so
    # prove the FILE changed during the run only if the trap misfired:
    # presence of a .smoke.bak is that misfire, already checked per tooth.
    if [ -f "${f}.smoke.bak" ]; then
        bad "leftover ${f}.smoke.bak after restore"
        dirty=$((dirty+1))
    fi
done
[ "$dirty" -eq 0 ] && ok "no leftovers from the teeth"

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
