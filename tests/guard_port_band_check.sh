#!/usr/bin/env bash
# tests/guard_port_band_check.sh — the guard port band must be configurable,
# must stay out of the live REST band, every allocation must be reserved until
# QEMU binds it, and no QEMU in the machinery may bind a literal host port.
#
# ─── Why this exists ───────────────────────────────────────────────────────
# Two incidents, one defect class: a test that picks its own host port picks
# the wrong one, and a wrong port does not fail — it tests a different machine.
#
#   * 2026-09-13. A live cluster held 3001..3003 during a deploy gate, so the
#     two boot checks with a hardcoded hostfwd port could not start QEMU at
#     all ("Could not set up host forwarding rule" went to /dev/null and
#     surfaced as "QEMU exited before the grub menu appeared").
#   * 2026-09-24. tests/free_port.sh scanned 3001..3020 — the live band — and
#     handed out 3001 because nothing answered the probe there. A local boot
#     check booted its kernel on production's port, and the CI
#     webapp_served_check.sh running on the same host compared THAT kernel's
#     assets against the committed bundle: a red 53-minute job with no cause
#     in the kernel, the guard, or the commit.
#
# Probing protects a port something ANSWERS on. It cannot protect a port
# something else TRUSTS without probing, and three things do: the default URL
# of tests/webapp_served_check.sh, the inline QEMU steps in ci.yml, and
# make x86-run (X86_HTTP_PORT). None of them can tell a boot-check kernel from
# the deployed one — both answer /api/health. So the rule this check enforces:
#
#   1. the band is configurable (AEROSLS_FREE_PORT_RANGE) and the default is
#      outside the live band: AEROSLS_HTTP_BASE+1 .. +CLUSTER_NODE_MAX, plus
#      the production backend on 3001;
#   2. an override that overlaps the live band is REFUSED, not clamped;
#   3. a malformed band is REFUSED, never silently replaced by the default
#      (a silently-ignored override hands QEMU a port nobody asked for);
#   4. a taken port is skipped and an exhausted band fails closed — returning
#      a taken port is how 2026-09-13 looked;
#   5. no QEMU hostfwd host port in the machinery is a literal (ci.yml,
#      Makefile, tests/*_check.sh). Documentation may show one (README's
#      copy-paste line, x86-run's documented default); the machinery may not.
#
# ─── The reservation, which probing cannot give you ────────────────────────
# A free port is a snapshot, not a claim. Two guard runs that both scan the
# band from the bottom both probe the same port, both find it free, and both
# hand it to their QEMU: one bind() wins, the other QEMU cannot set up its
# host forwarding rule and exits (2026-09-13, one process later). Nothing in a
# probe can prevent that, because the decision and the bind are seconds apart.
# So rule 6: tests/free_port.sh takes the port's lock file BEFORE probing it,
# keeps the lock in a detached holder until the port is bound (or
# AEROSLS_FREE_PORT_HOLD_S expires), and skips a locked port without probing
# it. These arms drive that directly — an allocation is reserved on the spot,
# a second allocation while it is reserved takes a different port, the
# reservation is released once the port is bound, and it cannot outlive its
# TTL. They use their own lock directory, so nothing they leave behind can
# take a port out of another run's band; the band arms above pass
# AEROSLS_FREE_PORT_HOLD=0 for the same reason.
#
# The teeth live in tests/guard_port_band_smoke.sh, which plants the
# mutations in the allocator: no lock test, no holder, a TTL ten times too
# long, a reserved port handed out anyway, and a missing flock that degrades
# to probe-only instead of refusing.
#
# Assertion 1 reads CLUSTER_NODE_MAX out of net/consensus.h, so raising the
# cap follows in this check instead of silently widening the live band past a
# stale literal. The live-cluster harnesses (tests/run_cluster_harness.sh &
# friends) legitimately name live ports — they assert what run-cluster.sh
# actually did — which is why the literal scan covers *_check.sh only.
#
# The skip/exhaustion arms need something to hold a port; that is python3 and
# a listener. Without it those arms cannot run, and an arm that did not run
# proves nothing, so it is an ABORT (exit 2) — the same posture
# tests/commands_doc_check.sh takes for the matchers it runs in python.
#
# Runs under tests/run_checks.sh (the tests/*_check.sh glob) on every push, and
# in deploy.sh's guard gate.
#
# Exit: 0 pass, 1 fail, 2 abort (python3, flock or free_port.sh missing).
set -u
cd "$(dirname "$0")/.."   # repo root

fail=0
pass=0
ok()  { echo "ok:   $1"; pass=$((pass + 1)); }
bad() { echo "FAIL: $1"; fail=$((fail + 1)); }

FREE_PORT=tests/free_port.sh
[ -f "$FREE_PORT" ] || { echo "ABORT: $FREE_PORT missing" >&2; exit 2; }
command -v python3 >/dev/null 2>&1 || {
    echo "ABORT: python3 not found — the skip/exhaustion arms need a listener." >&2
    exit 2
}
command -v flock >/dev/null 2>&1 || {
    echo "ABORT: flock not found — the reservation arms need it (util-linux)." >&2
    exit 2
}

# The live band's width follows the header, not a copy of the number here.
NODE_MAX="$(sed -n 's/^#define CLUSTER_NODE_MAX[[:space:]]\+\([0-9]\+\).*/\1/p' net/consensus.h 2>/dev/null | head -1)"
case "${NODE_MAX:-}" in ''|*[!0-9]*) NODE_MAX=8 ;; esac
PROD=3001                                  # the production backend (Caddy)

run_band() {   # run_band <range> [env...] -> stdout+stderr, rc in $?
    # Probe-only by default: these arms judge the band, they must not leave a
    # reservation behind for the next run (rule 6 tests the reservation, with
    # its own lock directory and its own TTLs).
    env AEROSLS_FREE_PORT_HOLD=0 "$@" bash "$FREE_PORT" 2>&1
}
run_plain() {  # run_plain [env...]
    env "$@" bash "$FREE_PORT" --print-band 2>&1
}

# ── 1. the default band is outside the live band ───────────────────────────
# Read the default out of free_port.sh's OWN SOURCE, not out of --print-band:
# a default that free_port refuses must still be judged on where it points.
# The refusal is a second, independent guard, and a check that only ever saw
# its abort text would report a formatting problem instead of naming the live
# port the default reaches, which is the thing a human has to fix.
default="$(sed -n 's/.*AEROSLS_FREE_PORT_RANGE:-\([0-9][0-9]*-[0-9][0-9]*\).*/\1/p' "$FREE_PORT" | head -1)"
case "$default" in
    [0-9]*-[0-9]*) ok "free_port.sh declares its default band as $default" ;;
    *) bad "could not read the AEROSLS_FREE_PORT_RANGE default out of $FREE_PORT (got '$default')"; default="" ;;
esac
if [ -n "$default" ]; then
    lo="${default%%-*}"; hi="${default#*-}"
    hit=""
    for p in $(seq "$PROD" $((PROD + NODE_MAX - 1))); do
        [ "$lo" -le "$p" ] && [ "$p" -le "$hi" ] && hit="$p"
    done
    if [ -n "$hit" ]; then
        bad "the default guard band $default contains live port $hit — a boot check would squat the live REST band"
    else
        ok "default guard band $default excludes the live band $PROD..$((PROD + NODE_MAX - 1)) (node cap $NODE_MAX, from net/consensus.h) and production $PROD"
    fi
    # With nothing overridden, the tool must resolve to exactly that default.
    resolved="$(run_plain)"
    if [ "$resolved" = "$default" ]; then
        ok "--print-band with nothing overridden resolves to the declared default"
    else
        bad "--print-band with nothing overridden printed '$resolved', not the declared default $default"
    fi
fi

# ── 2. the band follows AEROSLS_FREE_PORT_RANGE ────────────────────────────
printed="$(run_plain AEROSLS_FREE_PORT_RANGE=32111-32113)"
if [ "$printed" = "32111-32113" ]; then
    ok "AEROSLS_FREE_PORT_RANGE=32111-32113 is honoured (--print-band reports it back)"
else
    bad "AEROSLS_FREE_PORT_RANGE=32111-32113 printed '$printed', expected the override to move the band"
fi
alloc="$(run_band AEROSLS_FREE_PORT_RANGE=32111-32113)"
case "$alloc" in 32111|32112|32113) ok "the allocator stays inside the overridden band (handed out $alloc)";;
    *) bad "with band 32111-32113 the allocator handed out '$alloc', which is outside it";;
esac

# ── 3. a band that overlaps the live band is refused ───────────────────────
out="$(run_band AEROSLS_FREE_PORT_RANGE=$PROD-$((PROD + NODE_MAX - 1)))"; rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "live REST band"; then
    ok "a band equal to the live band ($PROD-$((PROD + NODE_MAX - 1))) is refused, naming the live band"
else
    bad "a band inside the live band was not refused (rc=$rc): $out"
fi
# The production backend alone: refused even when AEROSLS_HTTP_BASE points
# somewhere else, because production is the port something trusts.
out="$(run_band AEROSLS_HTTP_BASE=8000 AEROSLS_FREE_PORT_RANGE=$PROD)"; rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "production"; then
    ok "a band holding production $PROD alone is refused even with AEROSLS_HTTP_BASE=8000"
else
    bad "production's port alone was not refused with a moved base (rc=$rc): $out"
fi
out="$(run_band AEROSLS_HTTP_BASE=8000 AEROSLS_FREE_PORT_RANGE=8001-8008)"; rc=$?
if [ "$rc" -eq 1 ] && echo "$out" | grep -q "8001\.\.8008"; then
    ok "the live band follows AEROSLS_HTTP_BASE (8001-8008 with base 8000 is refused)"
else
    bad "the live band did not follow AEROSLS_HTTP_BASE=8000 (rc=$rc): $out"
fi

# ── 4. a malformed band is refused, not silently defaulted ─────────────────
for r in abc 32111-32100 32111-abc 0-5 70000-70005; do
    out="$(run_band AEROSLS_FREE_PORT_RANGE="$r")"; rc=$?
    if [ "$rc" -eq 1 ] && echo "$out" | grep -q "ABORT"; then
        ok "malformed band '$r' is refused with an explanation"
    else
        bad "malformed band '$r' was not refused (rc=$rc): $out"
    fi
done

# ── 5. a taken port is skipped; an exhausted band fails closed ─────────────
HOLD_A=32019
HOLD_B=32020
python3 - "$HOLD_A" "$HOLD_B" <<'PY' &
import socket, sys, time
socks = []
for arg in sys.argv[1:]:
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("127.0.0.1", int(arg)))
    s.listen(1)
    socks.append(s)
time.sleep(30)
PY
HOLDER=$!
trap 'kill "$HOLDER" 2>/dev/null || true' EXIT

held=0
for _ in $(seq 1 30); do
    if timeout 2 bash -c 'exec 3<>"/dev/tcp/127.0.0.1/$1"' _ "$HOLD_A" 2>/dev/null; then held=1; break; fi
    sleep 0.2
done
if [ "$held" -ne 1 ]; then
    bad "could not occupy $HOLD_A for the skip/exhaustion arms (another process holds it?) — arms not run"
else
    out="$(run_band AEROSLS_FREE_PORT_RANGE=$HOLD_A-$HOLD_B)"; rc=$?
    if [ "$rc" -eq 1 ] && echo "$out" | grep -q "no free loopback port"; then
        ok "an exhausted band ($HOLD_A-$HOLD_B, both held) fails closed instead of returning a taken port"
    else
        bad "an exhausted band did not fail closed (rc=$rc): $out"
    fi
    # Ten candidates, the last two held: the answer must be a free port in the
    # band and must not be one of the held ones. Ten, not one, so a concurrent
    # boot check holding a single port cannot make this arm flaky.
    out="$(run_band AEROSLS_FREE_PORT_RANGE=$((HOLD_A - 10))-$HOLD_B)"; rc=$?
    case "$out" in
        "$HOLD_A"|"$HOLD_B"|"") bad "a taken port was handed out (rc=$rc, band $((HOLD_A - 10))-$HOLD_B): '$out'";;
        *)
            if [ "$rc" -eq 0 ] && [ "$out" -ge $((HOLD_A - 10)) ] && [ "$out" -le "$HOLD_B" ]; then
                ok "a taken port is skipped (held $HOLD_A/$HOLD_B, handed out $out)"
            else
                bad "band $((HOLD_A - 10))-$HOLD_B with two held ports gave rc=$rc and '$out'"
            fi
            ;;
    esac
fi
kill "$HOLDER" 2>/dev/null || true
wait "$HOLDER" 2>/dev/null || true
trap - EXIT

# ── 6. every allocation is reserved until QEMU binds it ────────────────────
# The probe is a snapshot; the lock file is the claim. Each arm below asks
# tests/free_port.sh for a port and then looks at that port's lock file from
# the outside (flock -n), which is the only view that can tell "reserved" from
# "free right now".
P7DIR="$(mktemp -d)"
P7LOCKS="$P7DIR/locks"
trap 'rm -rf "$P7DIR"' EXIT

p7_lock_path() { AEROSLS_FREE_PORT_LOCK_DIR="$P7LOCKS" bash "$FREE_PORT" --lock-file "$1"; }
p7_lock_free() { flock -n "$1" true 2>/dev/null; }   # 0 = nobody holds it
p7_alloc() {   # p7_alloc <band> [env...] -> the port on stdout, stderr included
    local band=$1; shift
    env AEROSLS_FREE_PORT_RANGE="$band" AEROSLS_FREE_PORT_LOCK_DIR="$P7LOCKS" \
        AEROSLS_FREE_PORT_HOLD=1 "$@" bash "$FREE_PORT" 2>&1
}

# ── 6a. an allocation is reserved before its QEMU exists ───────────────────
# The first run allocates and is still up; the arm waits for its lock file to
# be held, which is the window (allocate -> bind) a probe could not cover.
# Small bands, not single ports: a stray listener anywhere in one just moves
# the allocation along, where a single-port band would red the arm for a
# reason that has nothing to do with the reservation.
P7A_LO=32061; P7A_HI=32065; P7A_BAND="$P7A_LO-$P7A_HI"
P7A_OUT="$P7DIR/a.out"
( AEROSLS_FREE_PORT_RANGE="$P7A_BAND" AEROSLS_FREE_PORT_LOCK_DIR="$P7LOCKS" \
  AEROSLS_FREE_PORT_HOLD_S=8 bash "$FREE_PORT" >"$P7A_OUT" 2>&1 ) &
P7A_PID=$!
p7a=""
for _ in $(seq 1 30); do
    p7a="$(head -1 "$P7A_OUT" 2>/dev/null || true)"
    [ -n "$p7a" ] && break
    sleep 0.1
done
wait "$P7A_PID" 2>/dev/null || true
p7_lk="$(p7_lock_path "${p7a:-0}")"
p7a_held=0
for _ in $(seq 1 20); do
    if ! p7_lock_free "$p7_lk"; then p7a_held=1; break; fi
    sleep 0.1
done
if [ -n "$p7a" ] && [ "$p7a_held" = 1 ]; then
    ok "the allocation $p7a is reserved on the spot (its lock is held while its QEMU is still starting)"
else
    bad "the allocation '${p7a:-<none>}' is not reserved — nothing stops a second run from taking the same port"
fi

# ── 6b. a second run takes a different port ────────────────────────────────
# Same band, while the first reservation is still held. This is the collision
# the probe cannot prevent: the port is free, and it is still not available.
p7b="$(p7_alloc "$P7A_BAND" AEROSLS_FREE_PORT_HOLD_S=8)"
if [ -n "$p7a" ] && [ "$p7b" != "$p7a" ] && [ "$p7b" -ge "$P7A_LO" ] && [ "$p7b" -le "$P7A_HI" ]; then
    ok "with $p7a reserved, a second allocation takes $p7b — the two cannot meet on QEMU's bind"
else
    bad "a second allocation handed out '${p7b:-<none>}' while '${p7a:-<none>}' was reserved (probe-only allocation)"
fi

# ── 6c. the reservation is released once the port is bound ─────────────────
P7C_BAND=32071-32074
p7c="$(p7_alloc "$P7C_BAND" AEROSLS_FREE_PORT_HOLD_S=20)"
p7c_lk="$(p7_lock_path "${p7c:-0}")"
p7c_held=0
[ -n "$p7c" ] && ! p7_lock_free "$p7c_lk" && p7c_held=1
if [ "$p7c_held" = 1 ]; then
    ok "the allocation $p7c is reserved (its lock is held before QEMU owns the port)"
else
    bad "the allocation '${p7c:-<none>}' came without a held reservation"
fi
python3 - "$p7c" <<'PY' &
import socket, sys, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("127.0.0.1", int(sys.argv[1])))
s.listen(1)
time.sleep(20)
PY
P7C_BINDER=$!
p7c_released=0
for _ in $(seq 1 25); do
    if p7_lock_free "$p7c_lk"; then p7c_released=1; break; fi
    sleep 0.2
done
kill "$P7C_BINDER" 2>/dev/null || true
wait "$P7C_BINDER" 2>/dev/null || true
if [ "$p7c_released" = 1 ]; then
    ok "the reservation is released as soon as the port is bound — it closes the probe's window, it does not sit on the band"
else
    bad "the reservation of '${p7c:-<none>}' was still held 5s after the port was bound (the holder did not see the bind)"
fi

# ── 6d. a reservation cannot outlive its TTL ───────────────────────────────
# A guard killed between allocating and launching QEMU must not remove a port
# from the band for good, so the holder is bounded even when nothing binds.
P7D_BAND=32075-32078
p7d="$(p7_alloc "$P7D_BAND" AEROSLS_FREE_PORT_HOLD_S=1)"
p7d_lk="$(p7_lock_path "${p7d:-0}")"
p7d_held=0
[ -n "$p7d" ] && ! p7_lock_free "$p7d_lk" && p7d_held=1
sleep 1.6
if [ "$p7d_held" = 1 ] && p7_lock_free "$p7d_lk"; then
    ok "HOLD_S=1 bounds the reservation: held when allocated, released 1.6s later with nothing bound"
else
    p7d_free=no; p7_lock_free "$p7d_lk" && p7d_free=yes
    bad "HOLD_S=1 did not bound the reservation (held at allocation: $p7d_held, released 1.6s later: $p7d_free)"
fi

# ── 6e. AEROSLS_FREE_PORT_HOLD=0 is honestly side-effect free ──────────────
# The band arms above rely on it, and so do hosts without util-linux's flock.
P7E_BAND=32081-32082
p7e1="$(run_band AEROSLS_FREE_PORT_RANGE=$P7E_BAND AEROSLS_FREE_PORT_LOCK_DIR="$P7LOCKS")"
p7e2="$(run_band AEROSLS_FREE_PORT_RANGE=$P7E_BAND AEROSLS_FREE_PORT_LOCK_DIR="$P7LOCKS")"
p7e_lk="$(p7_lock_path "${p7e1:-0}")"
if [ -n "$p7e1" ] && [ "$p7e1" = "$p7e2" ] && [ ! -e "$p7e_lk" ]; then
    ok "AEROSLS_FREE_PORT_HOLD=0 probes without reserving (both runs got $p7e1, no lock file)"
else
    bad "AEROSLS_FREE_PORT_HOLD=0 was not side-effect free (got '$p7e1' then '$p7e2', lock file $([ -e "$p7e_lk" ] && echo present || echo absent))"
fi

# ── 6f. no flock ABORTs rather than quietly probing only ───────────────────
# Probing alone is the defect rule 6 exists for, so it must not be the silent
# fallback on a host whose PATH has no flock: refuse, and name the escape.
P7_FAKEBIN="$P7DIR/nolock"
mkdir -p "$P7_FAKEBIN"
for tool in bash sed dirname; do ln -sf "$(command -v "$tool")" "$P7_FAKEBIN/$tool"; done
p7f="$(env PATH="$P7_FAKEBIN" AEROSLS_FREE_PORT_RANGE=32085-32085 bash "$FREE_PORT" 2>&1)"; p7f_rc=$?
# Both halves are the point: name the missing prerequisite (so the reader knows
# what to install) and name the escape hatch (so the reader knows their
# alternative). 'flock not found' is also what separates THIS refusal from the
# ones downstream of it — a mutation that removes the prerequisite check still
# exits 1 here, but with the mkdir or the band message instead.
if [ "$p7f_rc" -eq 1 ] && echo "$p7f" | grep -q 'flock not found' \
   && echo "$p7f" | grep -q 'AEROSLS_FREE_PORT_HOLD=0'; then
    ok "without flock the allocator refuses (exit 1), names flock, and names the escape hatch"
else
    bad "without flock the allocator did not refuse with flock named (rc=$p7f_rc): $p7f"
fi

rm -rf "$P7DIR"
trap - EXIT

# ── 7. no literal host port in the machinery ───────────────────────────────
lit="$(python3 - <<'PY'
import glob, os, re
# The scanner excludes ITSELF: it carries the pattern it searches for as text,
# so flagging its own source would be a red for the wrong reason. The real
# literal tooth is planted in a scratch check by tests/guard_port_band_smoke.sh.
me = os.path.normpath('tests/guard_port_band_check.sh')
files = ['.github/workflows/ci.yml', 'Makefile'] + \
        [f for f in sorted(glob.glob('tests/*_check.sh')) if os.path.normpath(f) != me]
for f in files:
    try:
        lines = open(f, errors='replace').read().splitlines()
    except OSError:
        continue
    for i, ln in enumerate(lines):
        if ln.lstrip().startswith('#'):
            continue
        for m in re.finditer(r'hostfwd=tcp:([^\s"\\]+?)-:', ln):
            if '$' not in m.group(1):
                print("%s:%d: %s" % (f, i + 1, ln.strip()))
PY
)"
if [ -z "$lit" ]; then
    ok "every QEMU hostfwd host port in ci.yml, the Makefile and tests/*_check.sh is an expansion"
else
    bad "literal host port(s) in a QEMU hostfwd — bind a probed port instead:"
    echo "$lit" | sed 's/^/        /'
fi

# Every QEMU in ci.yml allocates through the shared allocator: a fixed port in
# CI is the 2026-09-13 collision, and CI's own steps run on the same host as
# the guard jobs.
hf="$(grep -cE '^[[:space:]]*[^#[:space:]].*hostfwd=tcp:' .github/workflows/ci.yml || true)"
fp="$(grep -cE '^[[:space:]]*[^#[:space:]].*bash tests/free_port\.sh' .github/workflows/ci.yml || true)"
if [ "$hf" -gt 0 ] && [ "$hf" = "$fp" ]; then
    ok "ci.yml starts $hf QEMU(s), each taking its host port from tests/free_port.sh"
else
    bad "ci.yml has $hf hostfwd line(s) but $fp free_port.sh allocation(s) — every CI QEMU must allocate (and so reserve) its port"
fi

echo
echo "guard_port_band_check: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
