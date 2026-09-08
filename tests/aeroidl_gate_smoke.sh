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
# The fifth through tenth teeth are different: they do not break a
# generator but a CONSTANT, a STRUCT LAYOUT, the BUILD-TIME ABI gate,
# KERNEL-SIDE drift, a SYSCALL NUMBER, and the DISPATCH WIRING. Tooth 5
# flips CHAN_FLAG_NO_REPLY in the Rust mock (0x0001 -> 0x0002); tooth 6
# narrows `offset` in the runtime's CapDesc (u32 -> u16); tooth 8 widens
# `len` in the KERNEL's SLSCapDesc (u32 -> u64); tooth 9 bumps
# SYS_CAP_SEND_MSG (302 -> 303); tooth 10 renames a case label in
# syscall_dispatch.c — all five require the cross_lang_constants test to
# fail. Tooth 7 widens `pad` in FfiCapDescriptor (u16 -> u32) and requires
# the AEROSLS CRATE'S OWN cargo BUILD to fail on the compile-time const
# asserts in req.rs (nothing else reads `pad`, so only the build gate
# notices). The consistency test greps/parses files directly and the const
# asserts run at compile time, so no separate test binary is involved —
# cargo recompiles as needed. These prove the ABI gate bites from every
# direction: Ring-3 constants, Ring-3 wire layout, the compiled Rust
# layout, the kernel source, the syscall-number contract, and the dispatch
# wiring that turns a number into a handler.
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
MOCK_RUST=tools/aeroidl/tests/mock_aerosls/src/lib.rs
REQ_RUST=user/aerosls/src/req.rs
KERNEL_CAP_H=kernel/cap.h
KERNEL_DISPATCH=kernel/syscall_dispatch.c

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

    # cp -p so the snapshot carries the file's ORIGINAL mtime: the restore
    # below must put the mtime back too, not just the bytes. These teeth
    # mutate kernel sources (cap.h, syscall_dispatch.c); a restore that left
    # them newer than the linked kernel would poison every later
    # staleness-checked guard in the same CI job (stack_frame_budget
    # aborts with "binary is older than the sources" — seen live when this
    # smoke ran before it in kernel-guards).
    cp -p "$file" "$snap" || { bad "$name: cannot snapshot $file"; return; }
    # The trap restores every file even if this script dies mid-tooth.
    trap "mv -f \"$snap\" \"$file\" 2>/dev/null; case \"$file\" in *.rs) touch \"$file\";; esac; rm -f \"$snap\"" EXIT

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
    # touch makes the mutation visible to cargo's incremental build. The
    # restore side deliberately does NOT touch: mv of the cp -p snapshot
    # puts the original mtime back, which later staleness guards rely on.
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
    # mv preserves the snapshot's mtime (the original, thanks to cp -p).
    # Kernel sources (cap.h, syscall_dispatch.c) must get their ORIGINAL
    # mtime back: a restore that leaves them newer than the linked kernel
    # aborts the next staleness-checked guard in this CI job
    # (stack_frame_budget — seen live when this smoke ran before it).
    # But the .rs files cargo watches need a touch, or cargo's mtime
    # comparison sees the restored (older) mtime as "unchanged" and skips
    # rebuilding the binary the next rebuild() compiles — leaving the
    # tooth's mutation in the compiled compiler while the tree looks
    # restored. Touch only .rs; leave kernel sources' mtimes alone.
    mv -f "$snap" "$file"
    case "$file" in
        *.rs) touch "$file" ;;
    esac
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

# 5–7 mutate req.rs/mock sources and require a CHECK to fail: the
# cross_lang_constants test (tooth 5 constants, tooth 6 wire layout) or the
# aerosls crate's own cargo build (tooth 7 — the compile-time const asserts
# in req.rs, which fail the BUILD on layout drift).
# mutate_tooth <name> <file> <sed-mutation> <check-command>
# The check command is eval'd; a zero exit means the gate did not notice the
# drift and is blind. The command must not emit anything (stderr suppressed).
mutate_tooth() {
    local name="$1" file="$2" mutation="$3" check_cmd="$4"
    local snap; snap="${file}.smoke.bak"

    # cp -p: same mtime-preservation contract as tooth() above.
    cp -p "$file" "$snap" || { bad "$name: cannot snapshot $file"; return; }
    trap "mv -f \"$snap\" \"$file\" 2>/dev/null; case \"$file\" in *.rs) touch \"$file\";; esac; rm -f \"$snap\"" EXIT

    if ! sed -i "$mutation" "$file"; then
        bad "$name: sed failed to apply mutation"
        restore_checked "$name" "$file" "$snap"; return
    fi
    if cmp -s "$file" "$snap"; then
        bad "$name: mutation did not apply — the pattern no longer matches
            $file, so this smoke is testing nothing. Fix the pattern."
        restore_checked "$name" "$file" "$snap"; return
    fi
    touch "$file"
    ok "$name: mutation applied"

    if eval "$check_cmd" >/dev/null 2>&1; then
        bad "$name: the check did NOT fail on the drift. It is blind."
    else
        ok "$name: check failed as required (drift detected)"
    fi

    restore_checked "$name" "$file" "$snap"
}

# The check command for the consistency teeth: the cross-language test.
CONSISTENCY_CHECK="cargo test --quiet --test cross_lang_constants --manifest-path $MANIFEST"

# 5. Cross-language constants: flip CHAN_FLAG_NO_REPLY in the Rust mock. The
#    cross_lang_constants test greps every constant against kernel/cap.h and
#    must fail — this is the exact drift class the check exists for. No
#    compiler rebuild is needed: the mock is a test dependency, so `cargo
#    test` recompiles it automatically.
mutate_tooth "constants" "$MOCK_RUST" \
    's@pub const CHAN_FLAG_NO_REPLY: u16 = 0x0001;@pub const CHAN_FLAG_NO_REPLY: u16 = 0x0002;@' \
    "$CONSISTENCY_CHECK"

# 6. Wire struct layout: narrow `offset` in the runtime's CapDesc (u32 ->
#    u16). The cross_lang_constants layout engine computes offsets/sizes from
#    the file and must fail on the drift — which propagates into
#    SendMsgReq/RecvMsgReq too, since they embed the descriptor. (The first
#    `pub offset: u32,` in req.rs is CapDesc's; FfiCapDescriptor comes later.)
mutate_tooth "struct layout" "$REQ_RUST" \
    '0,/pub offset: u32,/s/pub offset: u32,/pub offset: u16,/' \
    "$CONSISTENCY_CHECK"

# 7. Build-time ABI gate: widen `pad` in FfiCapDescriptor (u16 -> u32). The
#    compile-time const asserts in req.rs make the AEROSLS CRATE'S OWN BUILD
#    fail (size_of::<FfiCapDescriptor>() == 16) — nothing else reads `pad`,
#    so the cross-language test still passes and only the build gate notices.
#    This proves the const asserts bite in CI, not just in a local build.
mutate_tooth "build-time ABI" "$REQ_RUST" \
    '0,/pub pad: u16,/s/pub pad: u16,/pub pad: u32,/' \
    "cargo build --quiet -p aerosls --manifest-path user/Cargo.toml"

# 8. Kernel-side drift: widen `len` in the KERNEL's SLSCapDesc (u32 -> u64).
#    The compile-time const asserts in req.rs cannot see cap.h (they pin the
#    Rust structs against hardcoded numbers), so the aerosls build stays
#    green; the drift is caught by the cross-language test, which parses the
#    kernel as the comparison baseline — kernel_wire_layouts_anchored fails
#    the moment the kernel deviates from the ABI table, and the descriptor
#    consistency checks fail the moment the SDK/mocks/generated copies
#    disagree with the mutated kernel. This proves the gate catches drift
#    from the KERNEL side, not just from the Ring-3 runtimes.
mutate_tooth "kernel-side drift" "$KERNEL_CAP_H" \
    '0,/uint32_t len;/s/uint32_t len;/uint64_t len;/' \
    "$CONSISTENCY_CHECK"

# 9. Syscall numbers: bump SYS_CAP_SEND_MSG in the Rust runtime (302 -> 303).
#    Only the runtime defines this name, so only the cross-language syscall
#    pin notices — it catches the exact class of drift that would make a
#    sidecar call the wrong kernel entry point.
mutate_tooth "syscall numbers" "$REQ_RUST" \
    '0,/SYS_CAP_SEND_MSG: u64 = 302;/s/SYS_CAP_SEND_MSG: u64 = 302;/SYS_CAP_SEND_MSG: u64 = 303;/' \
    "$CONSISTENCY_CHECK"

# 10. Dispatch wiring: rename a case label in the kernel's syscall dispatch.
#     The dispatch_cases_wired test requires every cap-family case label to
#     be a kernel/cap.h macro and every kernel syscall to reach a case — a
#     renamed case (or a syscall defined but never dispatched) fails it.
mutate_tooth "dispatch wiring" "$KERNEL_DISPATCH" \
    '0,/case SYS_SLS_CAP_SEND_MSG:/s/case SYS_SLS_CAP_SEND_MSG:/case SYS_SLS_CAP_SEND_MSX:/' \
    "$CONSISTENCY_CHECK"

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

# The consistency test must pass on the restored tree too.
if eval "$CONSISTENCY_CHECK" >/dev/null 2>&1; then
    ok "constants restored — the consistency test passes again"
else
    bad "the consistency test still fails after the teeth were removed"
fi

# The build-time ABI gate must pass on the restored tree too.
if cargo build --quiet -p aerosls --manifest-path user/Cargo.toml >/dev/null 2>&1; then
    ok "aerosls restored — the build-time ABI gate passes again"
else
    bad "the aerosls build still fails after the teeth were removed"
fi

# The four generator files + the mock + the runtime + the kernel header must
# be byte-identical to what the run started with — this smoke must never
# leave a footprint.
dirty=0
for f in "$GEN_RUST" "$GEN_DISP" "$GEN_LISP" "$GEN_C" "$MOCK_RUST" "$REQ_RUST" "$KERNEL_CAP_H" "$KERNEL_DISPATCH"; do
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
