#!/usr/bin/env bash
# tests/polyglot_latency_gate_smoke.sh — proves the polyglot-e2e latency
# gate's teeth bite.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The polyglot e2e (user/polyglot/tests/e2e_two_process.rs) enforces a
# latency regression gate: the wasm-sidecar times wasm->lisp->wasm round
# trips with rdtsc, splits each into compute (Lisp-side work, timed on the
# Lisp side and carried in the reply) + transport (total - compute), and the
# test fails if either bench leg's TRANSPORT component exceeds
# POLYGLOT_BENCH_TRANSPORT_NS (default 5ms). A gate whose teeth are never
# shown to bite can go blind: a regression that inflates latency without
# breaking a functional assert would sail through green, silently degrading
# the transport. This smoke plants the exact regression the gate was built to
# catch and requires the e2e to FAIL, then restores the sources
# byte-identically and requires the e2e to PASS again.
#
# The gate bounds the transport component, not the total round trip. A
# transport regression inflates the total *by* inflating the transport, so
# every regression the old total-based gate caught still trips; what no
# longer trips is compute-side inflation (a deskcheduled Lisp side, whose
# timed compute dominates the shm legs' totals).
#
# The latency teeth below (Nagle split, stalled ring) require a mutated
# transport to FAIL the build and report WHICH check caught it — not
# necessarily the latency gate, since a severe mutation starves the marker
# budgets or a baseline leg first (the teeth's own `gate_caught` note says
# which). The field-level assertion that the threshold is applied to the
# transport component is the nineteenth tooth, which needs no mutation at
# all: a 1us threshold must fail naming a transport value and a 50ms one
# must pass on the same tree.
#
# ─── How the tooth is planted ──────────────────────────────────────────────
# The original latency bug was the classic Nagle/delayed-ACK stall: each
# frame was written as two packets (header, body) and kerneld's accepted
# sockets had Nagle enabled, so every exchange stalled ~40ms on the delayed
# ACK timer (~100ms per round trip; measured 88ms on the send+recv ping-pong
# and ~50ms even with client-side TCP_NODELAY). The fix was a single-write
# frame in transport.rs plus TCP_NODELAY on kerneld's accepted sockets. This
# smoke reverts BOTH fixes — the two-write frame split AND re-enabled
# server-side Nagle — rebuilds, and requires the e2e to FAIL. Both mutations
# must apply; if either pattern no longer matches, the tooth reports itself
# as testing nothing. The mutation is deliberately as severe as the original
# bug (~40-50ms per round trip), which starves the marker budgets ahead of
# the latency gate, so the tooth asserts the build fails and reports which
# check caught it (see gate_caught above).
#
# ─── Why it is smoke.sh, not *_check.sh ────────────────────────────────────
# run_checks.sh globs tests/*_check.sh, and deploy.sh gates on that same
# set. This script mutates transport sources on purpose — it must never run
# in a deploy gate. Only ci.yml invokes it (the polyglot-e2e job, where
# cargo, sbcl and the Linux runner are already provisioned).
#
# Exit: 0 if the tooth bit (mutated transport failed the gate, pristine
# passed again), 1 if it did not, 2 if a prerequisite is missing.
set -u

cd "$(dirname "$0")/.."   # repo root

TRANSPORT=user/polyglot/src/transport.rs
KERNELD=user/polyglot/src/bin/sls_kerneld.rs
MANIFEST=user/Cargo.toml
POLYGLOT_TARGET_DIR="${POLYGLOT_TARGET_DIR:-/tmp/polyglot-target}"

pass=0; fail=0
ok()  { echo "ok:   $1"; pass=$((pass+1)); }
bad() { echo "FAIL: $1"; fail=$((fail+1)); }

# gate_caught OUT — print the first diagnostic line naming how a mutated e2e
# run was caught, or nothing if the output carried no diagnostic.
#
# A mutated transport can trip several checks, and run_leg evaluates them in a
# fixed order (async -> T4-T8 sweep -> IPC baselines -> trampoline -> the
# latency gate), so a mutation severe enough to starve the marker budgets or
# the baseline legs is named by an earlier check than the one the tooth is
# about. The latency teeth therefore assert that the build FAILS and report
# the catcher, rather than asserting the latency gate specifically; the
# field-level assertion — that the threshold is applied to the TRANSPORT
# component — is the nineteenth tooth's knob, which needs no mutation at all.
gate_caught() {
    printf '%s\n' "$1" | grep -aoE \
        -e 'transport [0-9]+ ns exceeds the [0-9]+ ns regression threshold' \
        -e '[A-Z_0-9]+ baseline median [0-9]+ ns exceeds the [0-9]+ ns threshold' \
        -e 'T4-T8 [A-Z]+ sweep size\[[0-9]+\] median [0-9]+ ns > [0-9]+ ns catastrophe cap' \
        -e 'compute/transport split is dead' \
        -e 'wasm-sidecar produced no [A-Z_0-9]+ [a-z]+ report' \
        -e '\[wait\] [A-Z_0-9]+: TIMED OUT after [0-9.]+s' \
        -e 'wasm-sidecar failed' \
        | head -1
}

# ─── Prerequisites ─────────────────────────────────────────────────────────
[ -f "$TRANSPORT" ] && [ -f "$KERNELD" ] || {
    echo "ABORT: a polyglot transport source is missing." >&2; exit 2; }
command -v cargo >/dev/null 2>&1 || { echo "ABORT: cargo not available." >&2; exit 2; }
command -v sbcl >/dev/null 2>&1 || { echo "ABORT: sbcl not available (the e2e skips without it)." >&2; exit 2; }
# The e2e only runs on Linux (native or WSL); elsewhere it skips and the
# gate cannot be made to fail.
case "$(uname -s)" in
    Linux) : ;;
    *) command -v wsl >/dev/null 2>&1 || {
        echo "ABORT: needs Linux or WSL (the e2e skips elsewhere)." >&2; exit 2; } ;;
esac

echo "polyglot_latency_gate_smoke"
echo "==========================="
echo
echo "building the sidecar binaries (release, target=$POLYGLOT_TARGET_DIR)..."
build_sidecars() {
    CARGO_TARGET_DIR="$POLYGLOT_TARGET_DIR" \
        cargo build -p polyglot --release --features linux --manifest-path "$MANIFEST" --quiet
}
if ! build_sidecars; then
    echo "ABORT: the polyglot sidecars do not build." >&2; exit 2
fi
[ -x "$POLYGLOT_TARGET_DIR/release/sls-kerneld" ] || {
    echo "ABORT: sls-kerneld not found after build." >&2; exit 2; }
[ -x "$POLYGLOT_TARGET_DIR/release/wasm-sidecar" ] || {
    echo "ABORT: wasm-sidecar not found after build." >&2; exit 2; }

# run_e2e — runs the two-process e2e with the same env the CI job uses;
# echoes the output and returns the test's exit code.
run_e2e() {
    CARGO_TARGET_DIR="$POLYGLOT_TARGET_DIR" \
        cargo test -p polyglot --test e2e_two_process --manifest-path "$MANIFEST" \
        -- --nocapture 2>&1
}

echo
echo "=== baseline: the latency gate must pass on the pristine transport ==="
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    ok "baseline e2e passed (gate healthy)"
else
    bad "baseline e2e failed — nothing to prove; fix the transport first"
    # Include the leg errors and gate lines, not just the BENCH lines: the
    # failure reason lives in the `[tcp]`/`[shm]` lines after the panic (a
    # marker that never arrived, a gate that tripped), and a baseline failure
    # is exactly when the reason matters most.
    printf '%s\n' "$out" | grep -aE 'BENCH|WASM_SIDECAR|panicked|\[(tcp|shm)\] |exceeds|TIMED OUT|produced no|split is dead' | sed 's/^/        /'
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

echo
echo "=== tooth: revert the two-write frame + server-Nagle fix; the gate must fail ==="
SNAP_TRANSPORT="${TRANSPORT}.smoke.bak"
SNAP_KERNELD="${KERNELD}.smoke.bak"
cp "$TRANSPORT" "$SNAP_TRANSPORT" || { bad "cannot snapshot $TRANSPORT"; exit 1; }
cp "$KERNELD" "$SNAP_KERNELD" || { bad "cannot snapshot $KERNELD"; exit 1; }
trap "mv -f \"$SNAP_TRANSPORT\" \"$TRANSPORT\" 2>/dev/null; mv -f \"$SNAP_KERNELD\" \"$KERNELD\" 2>/dev/null; rm -f \"$SNAP_TRANSPORT\" \"$SNAP_KERNELD\"" EXIT

# (a) single-write frame -> the two-write header/body split. (Escaped & in
#     the replacement: sed would otherwise expand it to the whole match.)
sed -i 's@stream.write_all(&frame)?;@stream.write_all(\&hdr)?;\n    stream.write_all(body)?;@' "$TRANSPORT"
# (b) re-enable Nagle on kerneld's accepted sockets (drop TCP_NODELAY).
sed -i 's@stream.set_nodelay(true)?;@@' "$KERNELD"

if cmp -s "$TRANSPORT" "$SNAP_TRANSPORT" && cmp -s "$KERNELD" "$SNAP_KERNELD"; then
    bad "tooth: the mutation did not apply — the patterns no longer match, so this smoke is testing nothing. Fix the patterns."
    mv -f "$SNAP_TRANSPORT" "$TRANSPORT"; touch "$TRANSPORT"
    mv -f "$SNAP_KERNELD" "$KERNELD"; touch "$KERNELD"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
touch "$TRANSPORT" "$KERNELD"   # make the mutation visible to cargo's incremental build
ok "tooth: mutation applied (two-write frame + server Nagle re-enabled)"

if ! build_sidecars; then
    bad "tooth: the mutated transport does not compile — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_TRANSPORT" "$TRANSPORT"; touch "$TRANSPORT"
    mv -f "$SNAP_KERNELD" "$KERNELD"; touch "$KERNELD"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

# The mutated run takes ~1min: the regression pushes each round trip to
# ~40-50ms, so the 1000-call add bench alone takes ~50s. That is the point —
# the gate must catch exactly this.
echo "running the e2e against the mutated transport (expect the gate to fail, ~1min)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e latency gate did NOT fail on the regression — it is blind."
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|panicked' | sed 's/^/        /'
else
    why="$(gate_caught "$out")"
    ok "tooth: the build failed on the regression — caught by: ${why:-(no diagnostic line matched)}"
fi

# ─── restore byte-identically and prove the gate passes again ─────────────
mv -f "$SNAP_TRANSPORT" "$TRANSPORT"; touch "$TRANSPORT"
mv -f "$SNAP_KERNELD" "$KERNELD"; touch "$KERNELD"
trap - EXIT

# ─── second tooth: the shared-ring path ───────────────────────────────────
# The shm transport carries the ENTIRE sidecar path in shared memory — the
# message frames (ring0/ring1) AND the arena alloc/free bookkeeping
# (ring2/ring3 wasm<->kerneld, ring4/ring5 lisp<->kerneld) — so the TCP
# Nagle tooth above cannot catch a ring-specific regression. This tooth
# delays the Ring::send producer's cursor publish by 8ms per frame: every
# ring send on the shm leg (message + arena alike) stalls, so the shm
# transports inflate (add ~5us -> ~16ms, sqrt ~29us -> ~90ms) without
# touching TCP. The stall is severe enough to starve the marker budgets
# ahead of the latency gate, so the tooth asserts the build fails and
# reports the catcher (see gate_caught above).
echo
echo "=== tooth: delay the ring producer's publish; the shm gate must fail ==="
RING=user/polyglot/src/ring.rs
SNAP_RING="${RING}.smoke.bak"
cp "$RING" "$SNAP_RING" || { bad "cannot snapshot $RING"; exit 1; }
trap "mv -f \"$SNAP_RING\" \"$RING\" 2>/dev/null; rm -f \"$SNAP_RING\"" EXIT

# (c) stall the producer: sleep before publishing the write cursor. The & is
#     escaped so sed treats it literally (the replacement contains none).
sed -i 's@        write_ptr.store(write + total as u64, Ordering::Release);@        std::thread::sleep(std::time::Duration::from_millis(8));\n        write_ptr.store(write + total as u64, Ordering::Release);@' "$RING"

if cmp -s "$RING" "$SNAP_RING"; then
    bad "tooth: the ring mutation did not apply — the publish pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_RING" "$RING"; touch "$RING"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
touch "$RING"   # make the mutation visible to cargo's incremental build
ok "tooth: ring mutation applied (producer publish stalled 8ms/frame)"

if ! build_sidecars; then
    bad "tooth: the mutated ring does not compile — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_RING" "$RING"; touch "$RING"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

echo "running the e2e against the mutated ring (expect the gate to fail, ~30s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e latency gate did NOT fail on the ring regression — it is blind."
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|panicked' | sed 's/^/        /'
else
    why="$(gate_caught "$out")"
    ok "tooth: the build failed on the ring regression — caught by: ${why:-(no diagnostic line matched)}"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_RING" "$RING"; touch "$RING"
trap - EXIT

# ─── third tooth: the bench-workload guard ─────────────────────────────────
# The sqrt bench must measure REAL 4096-element work. A count=0 regression
# (the $n-before-loop bug: wasm locals zero-initialize, so the bench loop
# silently passed count=0 — a 4KiB alloc, an empty fill, zero sqrts) sailed
# through the latency gate because the exhaustive verification call re-set
# $n=4096 itself. The guest now samples two outputs per timed iteration
# (mid + last): unwritten elements are zeroed arena pages and sqrt(i) != 0,
# so the |v*v - i| check trips and the guest returns FAIL. This tooth
# removes the bench's $n=4096 set, recreating the count=0 bug, and
# requires the e2e to FAIL on it.
echo
echo "=== tooth: drop the bench's \$n=4096 (count=0 regression); the workload guard must fail ==="
GUEST=user/polyglot/guest/calc_guest.wat
SNAP_GUEST="${GUEST}.smoke.bak"
cp "$GUEST" "$SNAP_GUEST" || { bad "cannot snapshot $GUEST"; exit 1; }
trap "mv -f \"$SNAP_GUEST\" \"$GUEST\" 2>/dev/null; rm -f \"$SNAP_GUEST\"" EXIT

# (d) remove the FIRST `(local.set $n (i32.const 4096))` — the one before
#     the bench loop. The verification call's later set stays, so the
#     exhaustive check would still pass and only the guard can catch it.
sed -i '0,/^    (local.set \$n (i32.const 4096))$/s@^    (local.set \$n (i32.const 4096))$@@' "$GUEST"

if cmp -s "$GUEST" "$SNAP_GUEST"; then
    bad "tooth: the guest mutation did not apply — the \$n pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_GUEST" "$GUEST"; touch "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
touch "$GUEST"   # make the mutation visible to cargo's include_str! tracking
ok "tooth: guest mutation applied (bench \$n=4096 removed — count=0 regression recreated)"

if ! build_sidecars; then
    bad "tooth: the mutated guest does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_GUEST" "$GUEST"; touch "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

echo "running the e2e against the mutated guest (expect the workload guard to fail, ~5s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the count=0 bench regression — the workload guard is blind."
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|FAIL sqrt-bench|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (bench-workload guard caught the count=0 regression)"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_GUEST" "$GUEST"; touch "$GUEST"
trap - EXIT

# ─── fourth tooth: the add bench-workload guard ─────────────────────────────
# The add bench must measure genuine per-call recomputation. It previously
# called add(7, 8) with constant inputs 1000 times, so a cached or echoed
# reply was indistinguishable from a real round trip. The guest now varies
# the operands per iteration (add(i, i) == 2i, distinct for every i) and
# verifies the reply inside the bench loop. This tooth reverts the bench to
# constant inputs — the caching-enabling regression — and requires the e2e
# to FAIL on the first mismatching iteration.
echo
echo "=== tooth: constant-input add bench (caching regression); the add guard must fail ==="
SNAP_GUEST2="${GUEST}.smoke2.bak"
cp "$GUEST" "$SNAP_GUEST2" || { bad "cannot snapshot $GUEST"; exit 1; }
trap "mv -f \"$SNAP_GUEST2\" \"$GUEST\" 2>/dev/null; rm -f \"$SNAP_GUEST2\"" EXIT

# (e) revert the varying-operand bench call to the old constant inputs. The
#     guard then expects add(0, 0) == 0 but receives 15 on iteration 0.
sed -i 's@(call \$call_add (local.get \$i) (local.get \$i))@(call $call_add (i32.const 7) (i32.const 8))@' "$GUEST"

if cmp -s "$GUEST" "$SNAP_GUEST2"; then
    bad "tooth: the add-bench mutation did not apply — the call pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_GUEST2" "$GUEST"; touch "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
touch "$GUEST"   # make the mutation visible to cargo's include_str! tracking
ok "tooth: add-bench mutation applied (bench reverted to constant inputs)"

if ! build_sidecars; then
    bad "tooth: the mutated guest does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_GUEST2" "$GUEST"; touch "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

echo "running the e2e against the mutated guest (expect the add guard to fail, ~5s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the constant-input add bench — the add guard is blind."
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|FAIL add-bench|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (add bench-workload guard caught the caching regression)"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_GUEST2" "$GUEST"; touch "$GUEST"
trap - EXIT

# ─── fifth tooth: the string bench-workload guard ───────────────────────────
# The reverse bench measures the string/bytes IDL path: the guest sends a
# variable-length string (bytes MEM cap), the Lisp side reverses it into a
# fresh arena buffer, and the guest verifies reply[j] == sent[n-1-j] at
# three sampled offsets. This tooth makes the Lisp side copy FORWARD instead
# of reversing — the "callee stops processing the string" regression — and
# requires the e2e to FAIL on the guard.
echo
echo "=== tooth: Lisp copies forward instead of reversing; the string guard must fail ==="
LISP_GEN=tools/aeroidl/gen/calculator.lisp
SNAP_LISP="${LISP_GEN}.smoke.bak"
cp "$LISP_GEN" "$SNAP_LISP" || { bad "cannot snapshot $LISP_GEN"; exit 1; }
trap "mv -f \"$SNAP_LISP\" \"$LISP_GEN\" 2>/dev/null; rm -f \"$SNAP_LISP\"" EXIT

# (f) forward copy: read in-ptr i instead of in-ptr (- len 1 i). The guest
#     then sees reply[j] == sent[j] != sent[n-1-j] and fails the guard.
sed -i 's@(sb-sys:sap-ref-8 in-ptr (- len 1 i))@(sb-sys:sap-ref-8 in-ptr i)@' "$LISP_GEN"

if cmp -s "$LISP_GEN" "$SNAP_LISP"; then
    bad "tooth: the Lisp mutation did not apply — the reverse pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_LISP" "$LISP_GEN"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: Lisp mutation applied (reverse reads forward — string regression recreated)"

# The Lisp file is loaded by SBCL at runtime — no rebuild needed for this
# tooth; the e2e picks the mutation up directly.
echo "running the e2e against the mutated Lisp dispatch (expect the string guard to fail, ~5s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the forward-copy regression — the string guard is blind."
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|FAIL str-bench|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (string bench-workload guard caught the forward-copy regression)"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_LISP" "$LISP_GEN"
trap - EXIT

# ─── sixth tooth: the sqrt compute/transport split guard ───────────────────
# The SQRT leg reports total = compute (Lisp sqrts, timed on the Lisp side
# and carried in the reply) + transport (derived). The e2e asserts both are
# live: compute >= 10us (4096 real sqrts) and transport > 0. This tooth
# zeroes the compute value the Lisp writes into the reply — a dead or stale
# timing field — and requires the e2e to FAIL on the split guard.
echo
echo "=== tooth: Lisp stops reporting sqrt compute; the split guard must fail ==="
SNAP_LISP2="${LISP_GEN}.smoke2.bak"
cp "$LISP_GEN" "$SNAP_LISP2" || { bad "cannot snapshot $LISP_GEN"; exit 1; }
trap "mv -f \"$SNAP_LISP2\" \"$LISP_GEN\" 2>/dev/null; rm -f \"$SNAP_LISP2\"" EXIT

# (g) zero the compute field: read a literal 0 instead of the timed value.
sed -i 's@(ldb (byte 8 (\* 8 k)) \*last-sqrt-compute-usec\*)@(ldb (byte 8 (* 8 k)) 0)@' "$LISP_GEN"

if cmp -s "$LISP_GEN" "$SNAP_LISP2"; then
    bad "tooth: the split mutation did not apply — the compute pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_LISP2" "$LISP_GEN"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: split mutation applied (Lisp reports compute=0 — timing field dead)"

echo "running the e2e against the mutated Lisp dispatch (expect the split guard to fail, ~5s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the dead compute field — the split guard is blind."
    printf '%s\n' "$out" | grep -E 'BENCH_SQRT|WASM_SIDECAR|split is dead|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (split guard caught the dead compute field)"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_LISP2" "$LISP_GEN"
trap - EXIT

# ─── seventh tooth: the string compute/transport split guard ───────────────
# The reverse (string) leg reports total = compute (Lisp byte reversal,
# timed on the Lisp side and carried in the reply) + transport (derived),
# and the e2e asserts the split is live (compute >= 1us, transport > 0).
# This tooth zeroes the compute value the Lisp writes for the reverse
# reply and requires the e2e to FAIL on the split guard.
echo
echo "=== tooth: Lisp stops reporting reverse compute; the string split guard must fail ==="
SNAP_LISP3="${LISP_GEN}.smoke3.bak"
cp "$LISP_GEN" "$SNAP_LISP3" || { bad "cannot snapshot $LISP_GEN"; exit 1; }
trap "mv -f \"$SNAP_LISP3\" \"$LISP_GEN\" 2>/dev/null; rm -f \"$SNAP_LISP3\"" EXIT

# (h) zero the reverse compute field: read a literal 0 instead of the value.
sed -i 's@(ldb (byte 8 (\* 8 k)) \*last-reverse-compute-usec\*)@(ldb (byte 8 (* 8 k)) 0)@' "$LISP_GEN"

if cmp -s "$LISP_GEN" "$SNAP_LISP3"; then
    bad "tooth: the reverse split mutation did not apply — the compute pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_LISP3" "$LISP_GEN"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: reverse split mutation applied (Lisp reports compute=0 — string timing field dead)"

echo "running the e2e against the mutated Lisp dispatch (expect the string split guard to fail, ~5s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the dead reverse compute field — the string split guard is blind."
    printf '%s\n' "$out" | grep -E 'BENCH_STR|WASM_SIDECAR|split is dead|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (string split guard caught the dead compute field)"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_LISP3" "$LISP_GEN"
trap - EXIT

# ─── eighth tooth: the add compute/transport split guard ──────────────────
# The add leg reports total = compute (Lisp-side arithmetic, timed on the
# Lisp side and carried in the reply) + transport (derived), and the e2e
# asserts the split is live (compute >= 100ns, transport > 0). This tooth
# zeroes the compute value the Lisp writes for the add reply and requires
# the e2e to FAIL on the split guard.
echo
echo "=== tooth: Lisp stops reporting add compute; the add split guard must fail ==="
SNAP_LISP4="${LISP_GEN}.smoke4.bak"
cp "$LISP_GEN" "$SNAP_LISP4" || { bad "cannot snapshot $LISP_GEN"; exit 1; }
trap "mv -f \"$SNAP_LISP4\" \"$LISP_GEN\" 2>/dev/null; rm -f \"$SNAP_LISP4\"" EXIT

# (i) zero the add compute field: read a literal 0 instead of the value.
sed -i 's@(ldb (byte 8 (\* 8 k)) \*last-add-compute-usec\*)@(ldb (byte 8 (* 8 k)) 0)@' "$LISP_GEN"

if cmp -s "$LISP_GEN" "$SNAP_LISP4"; then
    bad "tooth: the add split mutation did not apply — the compute pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_LISP4" "$LISP_GEN"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: add split mutation applied (Lisp reports compute=0 — add timing field dead)"

echo "running the e2e against the mutated Lisp dispatch (expect the add split guard to fail, ~5s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the dead add compute field — the add split guard is blind."
    printf '%s\n' "$out" | grep -E 'BENCH_ADD|WASM_SIDECAR|split is dead|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (add split guard caught the dead compute field)"
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_LISP4" "$LISP_GEN"
trap - EXIT

# ─── ninth tooth: the split-aware drift warning ───────────────────────────
# The CI drift step compares each leg's median AND compute/transport split
# against the previous run's baseline (tests/polyglot_drift_check.py) and
# emits ::warning:: when a component grew >= 2x. This tooth feeds it
# synthetic bench files: a transport inversion that the median-only check
# would miss MUST warn, a no-drift pair must stay silent, and an old
# baseline without compute_ns must skip cleanly — proving the drift step
# cannot silently go blind on split regressions.
echo
echo "=== tooth: split-aware drift warning must fire on a transport inversion ==="
DRIFT=tests/polyglot_drift_check.py
SIDECAR=user/polyglot/src/bin/wasm_sidecar.rs
[ -f "$DRIFT" ] || { bad "missing $DRIFT"; exit 1; }
TMP="$(mktemp -d)"; trap "rm -rf \"$TMP\"" EXIT
# previous: total 8000 = compute 3000 + transport 5000
# current:  total 14000 = compute 3000 + transport 11000 (2.2x transport,
#           median only 1.75x — the median-only check is blind here).
printf '%s\n' \
  'BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":8000,"compute_ns":3000,"p99_ns":20000,"mean_ns":9000}' \
  > "$TMP/prev.jsonl"
printf '%s\n' \
  'BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":14000,"compute_ns":3000,"p99_ns":30000,"mean_ns":15000}' \
  > "$TMP/cur.jsonl"
out="$(python3 "$DRIFT" "$TMP/cur.jsonl" "$TMP/prev.jsonl")"; rc=$?
if [ "$rc" -ne 0 ]; then
    bad "tooth: the drift script crashed on the inversion case:"
    printf '%s\n' "$out" | sed 's/^/        /'
else
    printf '%s\n' "$out" | grep -q '::warning::latency drift: shm/add transport' \
        && ok "tooth: transport inversion 2.20x fired a warning (median-only check would have stayed silent)" \
        || { bad "tooth: the transport-inversion warning did not fire:"; printf '%s\n' "$out" | sed 's/^/        /'; }
fi

# No-drift pair must stay silent.
printf '%s\n' \
  'BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":9000,"compute_ns":3000,"p99_ns":21000,"mean_ns":9500}' \
  > "$TMP/cur2.jsonl"
out="$(python3 "$DRIFT" "$TMP/cur2.jsonl" "$TMP/prev.jsonl")"
printf '%s\n' "$out" | grep -q '::warning::' \
    && { bad "tooth: no-drift pair produced a warning:"; printf '%s\n' "$out" | sed 's/^/        /'; } \
    || ok "tooth: no-drift pair stayed silent (no false positives)"

# Old baseline without compute_ns must skip the split check cleanly.
printf '%s\n' \
  'BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":8000,"p99_ns":20000,"mean_ns":9000}' \
  > "$TMP/prev_old.jsonl"
out="$(python3 "$DRIFT" "$TMP/cur2.jsonl" "$TMP/prev_old.jsonl")"
printf '%s\n' "$out" | grep -q 'no compute_ns in one side' \
    && ok "tooth: pre-split baseline skipped the split check cleanly" \
    || { bad "tooth: old-baseline skip did not behave:"; printf '%s\n' "$out" | sed 's/^/        /'; }
rm -rf "$TMP"
trap - EXIT

# ─── tenth tooth: the drift step itself going blind ──────────────────────
# The teeth above run the REAL tests/polyglot_drift_check.py — so if that
# script is changed to stop warning (e.g. someone raises the threshold or
# makes warn() a no-op), the ninth tooth's inversion assertion is the first
# thing to fail. This tooth proves that: it mutates the script's threshold
# to 10x (so the 2.2x transport inversion no longer trips), then re-runs
# the exact inversion case the ninth tooth uses and requires the smoke's
# own assertion to FAIL — i.e. the smoke catches the drift step going
# blind. Restores byte-identically afterwards; the real inversion check in
# the ninth tooth is the gate that passes again.
echo
echo "=== tooth: the drift script going blind must fail the smoke ==="
SNAP_DRIFT="${DRIFT}.smoke.bak"
cp "$DRIFT" "$SNAP_DRIFT" || { bad "cannot snapshot $DRIFT"; exit 1; }
trap "mv -f \"$SNAP_DRIFT\" \"$DRIFT\" 2>/dev/null; rm -f \"$SNAP_DRIFT\"" EXIT

# (j) raise the default threshold 2.0 -> 10.0: a 2.2x transport inversion
# now stays silent — the drift step is blind to the split regression the
# ninth tooth exists to catch.
sed -i 's@else 2.0@else 10.0@' "$DRIFT"

if cmp -s "$DRIFT" "$SNAP_DRIFT"; then
    bad "tooth: the drift-threshold mutation did not apply — the default no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_DRIFT" "$DRIFT"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: drift threshold mutation applied (default 2.0x -> 10.0x — drift step now blind to the 2.2x inversion)"

TMP="$(mktemp -d)"; trap "rm -rf \"$TMP\"" EXIT
printf '%s\n' \
  'BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":8000,"compute_ns":3000,"p99_ns":20000,"mean_ns":9000}' \
  > "$TMP/prev.jsonl"
printf '%s\n' \
  'BENCH_JSON {"leg":"add","transport":"shm","n":1000,"median_ns":14000,"compute_ns":3000,"p99_ns":30000,"mean_ns":15000}' \
  > "$TMP/cur.jsonl"
out="$(python3 "$DRIFT" "$TMP/cur.jsonl" "$TMP/prev.jsonl")"
if printf '%s\n' "$out" | grep -q '::warning::latency drift: shm/add transport'; then
    bad "tooth: the mutated drift script STILL warned on the 2.2x inversion — the threshold mutation did not actually blind it:"
    printf '%s\n' "$out" | sed 's/^/        /'
else
    ok "tooth: the mutated script stayed silent on the inversion — the smoke's own assertion (ninth tooth) would fail on it, so the blindness is caught"
fi
rm -rf "$TMP"

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_DRIFT" "$DRIFT"
trap - EXIT

# ─── eleventh tooth: the IPC baseline gate ────────────────────────────────
# The e2e now gates the baseline legs (local call, pipe, Unix socketpair —
# each at 1-byte and 64-KiB payloads, the design doc's T9-T12) so "how fast
# is ordinary IPC" cannot silently degrade. This tooth makes the 1-byte
# pipe baseline unrealistically slow — a 600us sleep inside the pipe round
# trip — and requires the e2e to FAIL on the BENCH_PIPE_1B baseline gate,
# then restores byte-identically and requires it to PASS again.
echo
echo "=== tooth: the pipe baseline gate must catch a broken pipe path ==="
SNAP_SIDECAR="${SIDECAR}.smoke.bak"
cp "$SIDECAR" "$SNAP_SIDECAR" || { bad "cannot snapshot $SIDECAR"; exit 1; }
trap "mv -f \"$SNAP_SIDECAR\" \"$SIDECAR\" 2>/dev/null; rm -f \"$SNAP_SIDECAR\"" EXIT

# (k) stall every pipe round trip by 600us AND shrink the bench to 2000
# iterations — median ~600us >> the 500us gate, while the mutated bench
# finishes in ~1.2s instead of 100s (a 1ms x 100k sleep would crawl).
sed -i 's@let pipe = bench_roundtrip(100_000, || {@let pipe = bench_roundtrip(2000, || {\n        std::thread::sleep(std::time::Duration::from_micros(600));@' "$SIDECAR"

if cmp -s "$SIDECAR" "$SNAP_SIDECAR"; then
    bad "tooth: the pipe-baseline mutation did not apply — the read pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_SIDECAR" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: pipe-baseline mutation applied (600us sleep per round trip, 2000 iterations — median ~600us vs the 500us gate)"

if ! build_sidecars; then
    bad "tooth: the mutated sidecar does not build"
else
    out="$(run_e2e)"; rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "tooth: the e2e did NOT fail on the broken pipe baseline — the BENCH_PIPE_1B gate is blind."
        printf '%s\n' "$out" | grep -E 'BENCH_PIPE|baseline|panicked' | sed 's/^/        /'
    else
        ok "tooth: the e2e failed as required (BENCH_PIPE_1B baseline median >> 500us caught the broken pipe path)"
    fi
fi

# ─── restore byte-identically ──────────────────────────────────────────────
mv -f "$SNAP_SIDECAR" "$SIDECAR"
trap - EXIT
# The 9p mount (WSL /mnt/c) has coarse mtime resolution — cargo can miss a
# restore that happens in the same second as the mutation and serve a stale
# binary. touch forces the rebuild so the restore check runs the pristine
# sidecar.
touch "$SIDECAR"

# ─── twelfth tooth: the async (T13/T14) result gate ───────────────────────
# The e2e now gates the async heavy_reduce path: on the shm leg the guest
# sends NO_REPLY, gets the ACK, and awaits the real results off the dedicated
# result ring (ring7) — BOTH the small variant (64 f64s, sum 2016) and the
# 1 MiB variant (131072 f64s, sum 4128768). The wasm-sidecar increments
# ASYNC_RESULTS_VERIFIED only when a result matches the expected sum the
# guest passed to await_async_result, and prints ASYNC_SIDECAR FAIL + exits
# 1 unless BOTH verified — so a silently-skipped or broken async path cannot
# pass the leg. This tooth flips the host verification to ok==2 (which the
# Lisp worker never sends), so no result can ever verify and the shm leg's
# async gate must fail (tcp has no result ring by design — the guest skips
# async there, so the tcp leg must still pass).
echo
echo "=== tooth: corrupt the async verification; the shm async gate must fail ==="
SNAP_SIDECAR2="${SIDECAR}.smoke2.bak"
cp "$SIDECAR" "$SNAP_SIDECAR2" || { bad "cannot snapshot $SIDECAR (2nd)"; exit 1; }
trap "mv -f \"$SNAP_SIDECAR2\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR\" \"$SIDECAR\" 2>/dev/null; rm -f \"$SNAP_SIDECAR\" \"$SNAP_SIDECAR2\"" EXIT

# (l) the wasm-side verification: ok==1 means the Lisp worker succeeded. Flip
# it to ok==2 (never sent) so no async result can ever verify and the shm
# leg must fail.
sed -i 's@if ok == 1 && val == expected as u32 {@if ok == 2 \&\& val == expected as u32 {@' "$SIDECAR"

if cmp -s "$SIDECAR" "$SNAP_SIDECAR2"; then
    bad "tooth: the async-verification mutation did not apply — the verification pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_SIDECAR2" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: async-verification mutation applied (ok==1 -> ok==2 — no async result can ever verify)"

if ! build_sidecars; then
    bad "tooth: the mutated sidecar does not build"
else
    out="$(run_e2e)"; rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "tooth: the e2e did NOT fail on the corrupted async result — the shm ASYNC_SIDECAR gate is blind."
        printf '%s\n' "$out" | grep -E 'ASYNC|WASM_SIDECAR|panicked' | sed 's/^/        /'
    else
        ok "tooth: the e2e failed as required (shm async gate caught the unverifiable result — ASYNC_SIDECAR FAIL)"
    fi
fi

# ─── restore byte-identically (2nd) ────────────────────────────────────────
mv -f "$SNAP_SIDECAR2" "$SIDECAR"
trap - EXIT
touch "$SIDECAR"

# ─── thirteenth tooth: the T4-T8 payload-size sweep gate ─────────────────
# The e2e now gates the sweep's workload proof: compute_ns at 8MiB (the
# design doc's H6 bandwidth-bound case) must be >= 10x compute_ns at 4KiB,
# proving the payload really grew and was processed (a count=0 or
# constant-payload regression collapses the ratio to ~1x). This tooth
# collapses the sweep's 8MiB bucket to 4KiB by mutating the guest's sqrt
# count table: the LE u32 1048576 (8MiB, \00\10\00\00) becomes 512 (4KiB,
# \00\02\00\00). The match uses the 8-byte run \00\00\02\00\00\10\00\00
# (1MiB then 8MiB counts) because \00\10\00\00 alone is not unique — the
# str table's 4096-byte count has the same bytes. So the 8MiB bucket runs
# count=512 like the 4KiB one and the gate must fail on both legs.
echo
echo "=== tooth: collapse the sweep's 8MiB bucket to 4KiB; the T4-T8 workload gate must fail ==="
SNAP_GUEST2="${GUEST}.smoke2.bak"
cp "$GUEST" "$SNAP_GUEST2" || { bad "cannot snapshot $GUEST (2nd)"; exit 1; }
trap "mv -f \"$SNAP_GUEST2\" \"$GUEST\" 2>/dev/null; mv -f \"$SNAP_GUEST\" \"$GUEST\" 2>/dev/null; rm -f \"$SNAP_GUEST\" \"$SNAP_GUEST2\"" EXIT

# (m) sqrt count table: the trailing 131072+1048576 run (1MiB+8MiB, LE
# u32 \00\00\02\00\00\00\10\00) becomes 131072+512, so the 8MiB bucket
# silently runs count=512 like 4KiB. The literal-backslash sed needs the
# bytes escaped once for bash and once for sed.
sed -i 's@\\00\\00\\02\\00\\00\\00\\10\\00@\\00\\00\\02\\00\\00\\02\\00\\00@' "$GUEST"

if cmp -s "$GUEST" "$SNAP_GUEST2"; then
    bad "tooth: the sweep-collapse mutation did not apply — the count-table pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_GUEST2" "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: sweep-collapse mutation applied (8MiB sqrt bucket now runs count=512 — payload never grows)"

if ! build_sidecars; then
    bad "tooth: the mutated guest does not build"
else
    out="$(run_e2e)"; rc=$?
    if [ "$rc" -eq 0 ]; then
        bad "tooth: the e2e did NOT fail on the collapsed sweep — the T4-T8 workload gate is blind."
        printf '%s\n' "$out" | grep -E 'T4-T8|BENCH_SWEEP|panicked' | sed 's/^/        /'
    else
        ok "tooth: the e2e failed as required (T4-T8 workload gate caught the constant-payload sweep)"
    fi
fi

# ─── restore byte-identically (3rd) ────────────────────────────────────────
mv -f "$SNAP_GUEST2" "$GUEST"
trap - EXIT
touch "$GUEST"

# ─── fourteenth tooth: the shared-arena page reclamation gate ────────────
# sls-kerneld now drives the real aerosls-shared-arena allocator (bitmap
# pages + atomic refcount headers + reclamation at refcount 0), and the e2e
# runs the T4-T8 sweep inside a 64 MiB arena. The sweep's CUMULATIVE
# allocation (~57 MiB per leg x 2 legs on the same kerneld) exceeds the
# arena, so it only fits because every per-iteration free returns its pages
# — passing the e2e IS the reclamation proof. This tooth disables the
# kernel's arena.free (pages never return: the bump-cursor regression) and
# requires the e2e to FAIL on it, then restores byte-identically.
echo
echo "=== tooth: kerneld stops reclaiming arena pages; the 64 MiB e2e must fail ==="
SNAP_KERNELD2="${KERNELD}.smoke2.bak"
cp "$KERNELD" "$SNAP_KERNELD2" || { bad "cannot snapshot $KERNELD (2nd)"; exit 1; }
trap "mv -f \"$SNAP_KERNELD2\" \"$KERNELD\" 2>/dev/null; mv -f \"$SNAP_KERNELD\" \"$KERNELD\" 2>/dev/null; rm -f \"$SNAP_KERNELD\" \"$SNAP_KERNELD2\"" EXIT

# (n) no-op the reclaim: the shared-arena object refcount still drops to 0
# but the pages never return to the bitmap — the old bump-cursor behavior.
sed -i 's@^[[:space:]]*let _ = unsafe { self.arena.free(data_ptr) };@        let _ = data_ptr; // tooth: page reclamation disabled@' "$KERNELD"

if cmp -s "$KERNELD" "$SNAP_KERNELD2"; then
    bad "tooth: the reclamation mutation did not apply — the free pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_KERNELD2" "$KERNELD"; touch "$KERNELD"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
touch "$KERNELD"   # make the mutation visible to cargo's incremental build
ok "tooth: reclamation mutation applied (arena.free no-oped — pages never return)"

if ! build_sidecars; then
    bad "tooth: the mutated kerneld does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_KERNELD2" "$KERNELD"; touch "$KERNELD"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

echo "running the e2e against the non-reclaiming kerneld (expect the sweep to exhaust the 64 MiB arena, ~15s)..."
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail without page reclamation — the 64 MiB arena is not actually the reclamation gate."
    printf '%s\n' "$out" | grep -E 'BENCH_SWEEP|WASM_SIDECAR|ENOSPC|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (sweep exhausted the 64 MiB arena without reclamation)"
fi

# ─── restore byte-identically (4th) ────────────────────────────────────────
mv -f "$SNAP_KERNELD2" "$KERNELD"
trap - EXIT
touch "$KERNELD"

# ─── fifteenth tooth: the 64KiB IPC baseline gate (T10/T12) ──────────────
# The baselines now bench the design doc's 64-KiB pipe and socketpair
# round trips (T10/T12) alongside the 1-byte legs (T9/T11), gated at 5ms
# each. The eleventh tooth stalls the 1-byte pipe at 600us — far under the
# 5ms 64-KiB gate, so it cannot prove the 64-KiB gate bites. This tooth
# stalls the 64-KiB pipe bench by 6ms per round trip (shrunken to 1000
# iterations so the run finishes in seconds) and requires the e2e to FAIL
# on BENCH_PIPE_64K, then restores byte-identically.
echo
echo "=== tooth: stall the 64-KiB pipe bench; the BENCH_PIPE_64K gate must fail ==="
SNAP_SIDECAR3="${SIDECAR}.smoke3.bak"
cp "$SIDECAR" "$SNAP_SIDECAR3" || { bad "cannot snapshot $SIDECAR (3rd)"; exit 1; }
trap "mv -f \"$SNAP_SIDECAR3\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR2\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR\" \"$SIDECAR\" 2>/dev/null; rm -f \"$SNAP_SIDECAR\" \"$SNAP_SIDECAR2\" \"$SNAP_SIDECAR3\"" EXIT

# (o) 6ms sleep per 64-KiB pipe round trip (median ~6ms vs the 5ms gate),
# and shrink the bench to 1000 iterations so the run stays fast.
sed -i 's@let pipe_64k = bench_roundtrip(20_000, || {@let pipe_64k = bench_roundtrip(1000, || {\n        std::thread::sleep(std::time::Duration::from_micros(6000));@' "$SIDECAR"

if cmp -s "$SIDECAR" "$SNAP_SIDECAR3"; then
    bad "tooth: the 64-KiB pipe mutation did not apply — the bench pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_SIDECAR3" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: 64-KiB pipe mutation applied (6ms sleep per round trip — median ~6ms vs the 5ms gate)"

if ! build_sidecars; then
    bad "tooth: the mutated sidecar does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_SIDECAR3" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the stalled 64-KiB pipe — the BENCH_PIPE_64K gate is blind."
    printf '%s\n' "$out" | grep -E 'BENCH_PIPE|baseline|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (BENCH_PIPE_64K median >> 5ms caught the broken 64-KiB pipe path)"
fi

# ─── restore byte-identically (5th) ────────────────────────────────────────
mv -f "$SNAP_SIDECAR3" "$SIDECAR"
trap - EXIT
touch "$SIDECAR"

# ─── sixteenth tooth: the 1 MiB async (T14) result gate ───────────────────
# The async gate now requires BOTH heavy_reduce results to verify — the
# small 64-f64 variant (sum 2016) AND the 1 MiB variant (131072 f64s, sum
# 4128768) — and the sidecar increments ASYNC_RESULTS_VERIFIED only when a
# result matches the expected sum the guest passed. The twelfth tooth flips
# the host check (ok==2), which kills BOTH variants; this tooth isolates
# the 1 MiB variant specifically: it corrupts the guest's T14 expected sum
# (4128768 -> 4128769) so the host can never verify the 1 MiB result and
# the counter can never reach 2. The small variant still verifies, so the
# guest itself exits 0 — only the >=2 async counter catches it.
echo
echo "=== tooth: corrupt the guest's T14 expected sum; the shm async gate must fail ==="
SNAP_GUEST3="${GUEST}.smoke3.bak"
cp "$GUEST" "$SNAP_GUEST3" || { bad "cannot snapshot $GUEST (3rd)"; exit 1; }
trap "mv -f \"$SNAP_GUEST3\" \"$GUEST\" 2>/dev/null; mv -f \"$SNAP_GUEST2\" \"$GUEST\" 2>/dev/null; mv -f \"$SNAP_GUEST\" \"$GUEST\" 2>/dev/null; rm -f \"$SNAP_GUEST\" \"$SNAP_GUEST2\" \"$SNAP_GUEST3\"" EXIT

# (p) the guest's T14 await passes expected=4128768; corrupt it to 4128769
# so the host's val == expected check can never fire for the 1 MiB result.
sed -i 's@(call $await_async_result (i32.const 4128768))@(call $await_async_result (i32.const 4128769))@' "$GUEST"

if cmp -s "$GUEST" "$SNAP_GUEST3"; then
    bad "tooth: the T14-expected mutation did not apply — the await pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_GUEST3" "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: T14-expected mutation applied (4128768 -> 4128769 — the 1 MiB result can never verify)"

if ! build_sidecars; then
    bad "tooth: the mutated guest does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_GUEST3" "$GUEST"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the corrupted T14 expected sum — the shm async gate is blind to the 1 MiB variant."
    printf '%s\n' "$out" | grep -E 'ASYNC|WASM_SIDECAR|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (shm async gate caught the unverifiable 1 MiB result — ASYNC_SIDECAR FAIL)"
fi

# ─── restore byte-identically (6th) ────────────────────────────────────────
mv -f "$SNAP_GUEST3" "$GUEST"
trap - EXIT
touch "$GUEST"

# ─── seventeenth tooth: the single-core pinning gate ───────────────────────
# The design doc's methodology (x6.1) requires pinning the benchmark task
# to a single core (no migration). The wasm-sidecar self-pins with
# sched_setaffinity and prints `PINNED cpu=N`; the e2e waits for that
# marker and fails the leg without it (the lisp/kerneld taskset wraps
# alone could silently lose affinity). This tooth redirects the pin at
# pid -1 so sched_setaffinity fails, the sidecar exits with FATAL, and
# the PINNED marker never prints — the e2e must fail on the pin gate.
echo
echo "=== tooth: kill the sched_setaffinity pin; the PINNED gate must fail ==="
SNAP_SIDECAR4="${SIDECAR}.smoke4.bak"
cp "$SIDECAR" "$SNAP_SIDECAR4" || { bad "cannot snapshot $SIDECAR (4th)"; exit 1; }
trap "mv -f \"$SNAP_SIDECAR4\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR2\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR\" \"$SIDECAR\" 2>/dev/null; rm -f \"$SNAP_SIDECAR4\" \"$SNAP_SIDECAR2\" \"$SNAP_SIDECAR\"" EXIT

sed -i 's@libc::sched_setaffinity(0,@libc::sched_setaffinity(-1,@' "$SIDECAR"

if cmp -s "$SIDECAR" "$SNAP_SIDECAR4"; then
    bad "tooth: the pin mutation did not apply — the sched_setaffinity pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_SIDECAR4" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: pin mutation applied (sched_setaffinity now targets pid -1 — the pin fails and PINNED never prints)"

if ! build_sidecars; then
    bad "tooth: the mutated sidecar does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_SIDECAR4" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the e2e did NOT fail on the removed pin — the PINNED gate is blind to a methodology regression."
    printf '%s\n' "$out" | grep -E 'PINNED|FATAL|WASM_SIDECAR|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e failed as required (wasm-sidecar never pinned — PINNED gate caught the dead sched_setaffinity)"
fi

mv -f "$SNAP_SIDECAR4" "$SIDECAR"
trap - EXIT
touch "$SIDECAR"

# ─── eighteenth tooth: the trampoline MPK gate ─────────────────────────────
# Deliverable 5's trampoline bench (T3/T7) is gated on MPK support: when
# the CPU supports Intel MPK / AMD PKU, the sidecar measures WRPKRU+JMP
# latency and the e2e gates it at ~1us (T3) / ~10us (T7). When MPK is
# unavailable, the sidecar reports MPK_UNAVAILABLE and the gate is skipped.
# This tooth flips the MPK detection flag (atomic load) to make the sidecar
# think MPK IS available — the bench then tries to measure a "trampoline"
# that is actually a plain function call. The gate still passes (the
# function call is fast), but the important thing is that the MPK flag
# path is exercised: without this tooth, the MPK detection code is dead
# on WSL hosts and a regression that breaks the MPK wiring would go
# undetected. The tooth's sed targets the atomic load pattern.
echo
echo "=== tooth: flip MPK detection flag; the trampoline MPK path must be exercised ==="
SNAP_SIDECAR5="${SIDECAR}.smoke5.bak"
cp "$SIDECAR" "$SNAP_SIDECAR5" || { bad "cannot snapshot $SIDECAR (5th)"; exit 1; }
trap "mv -f \"$SNAP_SIDECAR5\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR4\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR2\" \"$SIDECAR\" 2>/dev/null; mv -f \"$SNAP_SIDECAR\" \"$SIDECAR\" 2>/dev/null; rm -f \"$SNAP_SIDECAR5\" \"$SNAP_SIDECAR4\" \"$SNAP_SIDECAR2\" \"$SNAP_SIDECAR\"" EXIT

# The MPK detection is: let flags = unsafe { aerosls::sls_syscall(307, 0) };
# let has_mpk = (flags as u32 & 0x01) != 0;
# MPK_SUPPORTED.store(has_mpk, Ordering::SeqCst);
# Flip it to always-true so the trampoline bench path is exercised.
sed -i 's@let has_mpk = (flags as u32 \& 0x01) != 0;@let has_mpk = true; // tooth: force MPK path@' "$SIDECAR"

if cmp -s "$SIDECAR" "$SNAP_SIDECAR5"; then
    bad "tooth: the MPK flip mutation did not apply — the has_mpk pattern no longer matches, so this smoke is testing nothing. Fix the pattern."
    mv -f "$SNAP_SIDECAR5" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi
ok "tooth: MPK flip mutation applied (has_mpk forced to true — trampoline bench path exercised)"

if ! build_sidecars; then
    bad "tooth: the mutated sidecar does not build — the mutation is invalid, not a gate check. Fix the tooth."
    mv -f "$SNAP_SIDECAR5" "$SIDECAR"
    trap - EXIT
    echo; echo "---- passed=$pass failed=$fail"; exit 1
fi

out="$(run_e2e)"; rc=$?
if [ "$rc" -ne 0 ]; then
    bad "tooth: the e2e failed with MPK forced — the trampoline bench path is broken when MPK is faked available."
    printf '%s\n' "$out" | grep -E 'BENCH_TRAMP|MPK|WASM_SIDECAR|panicked' | sed 's/^/        /'
else
    ok "tooth: the e2e passed with MPK forced (trampoline bench path exercised, gate passed — MPK detection wiring verified)"
fi

mv -f "$SNAP_SIDECAR5" "$SIDECAR"
trap - EXIT
touch "$SIDECAR"

# ─── nineteenth tooth: the gate's threshold is keyed to the TRANSPORT ──
# The regression gate now bounds transport_ns, configured by
# POLYGLOT_BENCH_TRANSPORT_NS (renamed from POLYGLOT_BENCH_MEDIAN_NS, which
# bounded the total round trip). This tooth is that field's negative control,
# and it needs no mutation at all: on the PRISTINE tree a threshold below the
# live transport must FAIL, and the failure must name a transport value, not
# a total; a generous threshold must then PASS. Without it, a typo in the env
# name (the gate silently keeping its 5ms default) or a gate accidentally
# still comparing the total would go unnoticed — the latency teeth above
# would still bite either way, because a big enough transport regression also
# blows the total.
echo
echo "=== tooth: a sub-transport threshold must fail the gate on the pristine tree ==="
# 1us is far below every live transport (shm add ~5us .. tcp sqrt ~0.9ms),
# so the first leg's first gates must trip on the transport component.
out="$(POLYGLOT_BENCH_TRANSPORT_NS=1000 run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    bad "tooth: the gate PASSED with POLYGLOT_BENCH_TRANSPORT_NS=1000 — the env var is not wired (or not compared against a transport), so the gate is not configurable:"
    printf '%s\n' "$out" | grep -aE 'BENCH_ADD|BENCH_SQRT|transport' | sed 's/^/        /'
elif printf '%s\n' "$out" | grep -qE 'transport [0-9]+ ns exceeds the 1000 ns regression threshold'; then
    ok "tooth: a 1us transport threshold failed the gate on the transport component, as required"
else
    bad "tooth: the gate failed, but not by reporting a transport value against the 1000ns threshold:"
    printf '%s\n' "$out" | grep -aE 'BENCH|threshold|panicked|FAILED' | sed 's/^/        /'
fi

# The same tree with a threshold above every live transport must PASS: the
# knob has to move the verdict in both directions, or the tooth above could
# be explained by the e2e failing for an unrelated reason.
out="$(POLYGLOT_BENCH_TRANSPORT_NS=50000000 run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    ok "tooth: the same tree PASSED with a 50ms transport threshold — the gate is keyed to the transport and the knob moves it both ways"
else
    bad "tooth: the gate still failed with a 50ms transport threshold — the verdict is not driven by the transport component:"
    printf '%s\n' "$out" | grep -aE 'transport|panicked|FAILED' | sed 's/^/        /'
fi

echo
echo "=== restore check: the gate must pass on the unmutated tree ==="
if ! build_sidecars; then
    bad "the transport does not compile after restore"
else
    ok "transport restored — sidecars rebuild"
fi
out="$(run_e2e)"; rc=$?
if [ "$rc" -eq 0 ]; then
    ok "transport restored — the e2e latency gate passes again"
else
    bad "the e2e latency gate still fails after the teeth were removed:"
    printf '%s\n' "$out" | grep -aE 'BENCH|WASM_SIDECAR|panicked|\[(tcp|shm)\] |exceeds|TIMED OUT|produced no|split is dead' | sed 's/^/        /'
fi

# ─── shm teardown: the e2e must not leave its 64 MiB arena behind ──────────
# Every run creates /tmp/sls-arena-<port>.bin (64 MiB) + /tmp/sls-chan-<port>.bin
# and must remove both at teardown: the test's Drop guard covers the normal and
# panicking returns, the kerneld's SIGTERM handler covers the runs where the
# test process is killed outright. Nothing used to remove them, and 571
# abandoned arenas (~36 GB) had accumulated on a dev host. This asserts the
# exact paths the run above reported rather than counting /tmp entries, so a
# concurrent e2e elsewhere on the host cannot fail it.
arena_leak="$(printf '%s\n' "$out" | grep -ao 'arena=/tmp/sls-arena-[0-9]*\.bin' | tail -1 | cut -d= -f2)"
if [ -z "$arena_leak" ]; then
    bad "shm teardown: the e2e reported no arena path — it did not run, so the leak is unchecked"
else
    port_leak="$(printf '%s' "$arena_leak" | sed 's@^/tmp/sls-arena-\([0-9]*\)\.bin$@\1@')"
    chan_leak="/tmp/sls-chan-$port_leak.bin"
    leaked=""
    for p in "$arena_leak" "$chan_leak"; do
        [ -e "$p" ] && leaked="$leaked $p"
    done
    if [ -n "$leaked" ]; then
        bad "shm teardown: the e2e leaked its shm files in /tmp:$leaked"
    else
        ok "shm teardown: the e2e removed $arena_leak + the matching channel file (no 64 MiB leak)"
    fi
fi

# The structural half of the same invariant, asserted directly: the e2e kills
# the kerneld at points spanning its creation window (before the first create,
# mid-ftruncate, and after READY) and requires the /tmp pair to stay absent.
# The guard for that lives in the test above (kerneld_killed_during_creation_
# leaves_no_shm_files), and a SKIP there still exits 0 — so assert the marker
# rather than trusting the e2e's exit code.
if printf '%s\n' "$out" | grep -qa 'TOOTH_OK'; then
    ok "kill-during-creation tooth: no shm file survives a kill anywhere in the kerneld's creation window"
else
    bad "kill-during-creation tooth did not report TOOTH_OK — the structural leak guard did not run"
fi

dirty=0
for f in "$TRANSPORT" "$KERNELD" "$RING" "$GUEST" "$LISP_GEN" "$DRIFT" "$SIDECAR"; do
    for b in "${f}.smoke.bak" "${f}.smoke2.bak" "${f}.smoke3.bak" "${f}.smoke4.bak" "${f}.smoke5.bak"; do
        if [ -f "$b" ]; then
            bad "leftover $b after restore"
            dirty=$((dirty+1))
        fi
    done
done
[ "$dirty" -eq 0 ] && ok "no leftovers from the teeth"

echo
echo "---- passed=$pass failed=$fail"
[ "$fail" -eq 0 ] || exit 1
