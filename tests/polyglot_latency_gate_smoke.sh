#!/usr/bin/env bash
# tests/polyglot_latency_gate_smoke.sh — proves the polyglot-e2e latency
# gate's teeth bite.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# The polyglot e2e (user/polyglot/tests/e2e_two_process.rs) enforces a
# latency regression gate: the wasm-sidecar times wasm->lisp->wasm round
# trips with rdtsc and the test fails if either bench leg's median exceeds
# POLYGLOT_BENCH_MEDIAN_NS (default 5ms). A gate whose teeth are never shown
# to bite can go blind: a regression that inflates latency without breaking
# a functional assert would sail through green, silently degrading the
# transport. This smoke plants the exact regression the gate was built to
# catch and requires the e2e to FAIL, then restores the sources
# byte-identically and requires the e2e to PASS again.
#
# ─── How the tooth is planted ──────────────────────────────────────────────
# The original latency bug was the classic Nagle/delayed-ACK stall: each
# frame was written as two packets (header, body) and kerneld's accepted
# sockets had Nagle enabled, so every exchange stalled ~40ms on the delayed
# ACK timer (~100ms per round trip; measured 88ms on the send+recv ping-pong
# and ~50ms even with client-side TCP_NODELAY). The fix was a single-write
# frame in transport.rs plus TCP_NODELAY on kerneld's accepted sockets. This
# smoke reverts BOTH fixes — the two-write frame split AND re-enabled
# server-side Nagle — rebuilds, and requires the e2e latency gate to fail
# (median >> 5ms). Both mutations must apply; if either pattern no longer
# matches, the tooth reports itself as testing nothing.
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
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|panicked' | sed 's/^/        /'
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
    ok "tooth: the latency gate failed as required (median blew past the threshold)"
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
# ring send on the shm leg (message + arena alike) stalls, inflating the
# shm medians (add ~2us -> ~16ms, sqrt -> ~90ms) without touching TCP. The
# gate must fail on it.
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
    ok "tooth: the latency gate failed as required (shm median blew past the threshold)"
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
    printf '%s\n' "$out" | grep -E 'BENCH|WASM_SIDECAR|panicked' | sed 's/^/        /'
fi

dirty=0
for f in "$TRANSPORT" "$KERNELD" "$RING" "$GUEST"; do
    for b in "${f}.smoke.bak" "${f}.smoke2.bak"; do
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
