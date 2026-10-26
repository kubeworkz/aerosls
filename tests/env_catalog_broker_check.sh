#!/usr/bin/env bash
# tests/env_catalog_broker_check.sh — POSIX-Environments v0.2 §9.1's guard:
# the five wires that make kernel.env.catalog reachable at all, plus the
# boundary that section exists to establish.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# §9.1 answers the project's largest open architectural question with a
# read-only, partition-scoped view of the object catalog, brokered by a
# second kernel-owned service — and then states how that answer is verified:
#
#     "kernel/env_catalog.c compiles into the kernel; the service is
#      registered by cap_create_sidecar() under the kernel-owned name
#      kernel.env.catalog, its kernel slot is skipped by
#      console_service_tick() so CAT_* frames never reach the shared serial
#      transcript, and its tick runs in microkernel_service_poll() beside
#      net_socket_service_tick(). tests/env_catalog_broker_check.sh guards
#      each of those five wires plus the boundary itself."
#
# Each of the five is a WIRE, and wires are exactly what the host test
# cannot see: tests/env_catalog_host_test.c executes all four verbs against
# the real service and would stay green while the service was unregistered,
# never ticked, or having its frames eaten by the console drain — because
# none of those are things a test that calls the tick itself can observe.
# This guard pins the four files that must agree for the verbs to be
# reachable in a boot, then the boundary in the two places that enforce it
# and the one place that proves it.
#
# ─── The clauses ──────────────────────────────────────────────────────────
#   W1. the Makefile compiles kernel/env_catalog.c into the kernel;
#   W2. cap_create_sidecar() REACHES the registration — the two-hop chain
#       (cap_create_sidecar -> cap_create_sidecar_in ->
#       cap_create_sidecar_in_personality) and the register call itself;
#   W3. the arm is gated on the KERNEL-owned name: after the "kernel."
#       branch opens, before the tenant-resolvable registry fallback, and
#       the service answers under that same name in its refusals;
#   W4. console_service_tick() skips the channel's kernel slot — printing a
#       CAT_* frame would leak a tenant's object names onto the shared
#       serial transcript AND steal the frame the tick must answer;
#   W5. the tick runs inside microkernel_service_poll(), after
#       net_socket_service_tick() — the same single-writer context, which
#       is the whole reason this service takes no lock of its own;
#   B1. THE BOUNDARY: every verb is filtered on the partition the channel
#       REGISTERED with (cap.c's pd->partition_id) — as ABSENCE, a foreign
#       entry is skipped rather than denied — and nothing on the wire can
#       move that partition: the only store of a live value is the register
#       argument, and every filter reads e->partition;
#   B2. the boundary's evidence: the host test links the REAL service,
#       pins the indistinguishable-NOENT clause, and is actually built by
#       run_all.sh — a test the runner skips is a boundary guarded by
#       nothing, which is the failure run_all.sh's own header exists for.
#
# ─── What this guard does NOT do ──────────────────────────────────────────
# It has no live arm: §9.1 names a source guard, and the verbs are executed
# by the host test. A committed smoke that reddens each tooth below when the
# property is removed is NOT written yet — said here rather than implied, so
# that nobody reads the teeth above as proven when only the green run has
# been. Each anchor is a CODE line rather than a comment (the N12 rule), so
# prose cannot satisfy one.
#
# GUARD-KIND: host (plain bash over the tree; no build, no QEMU).
#
# Exit: 0 if every clause held, 1 if one failed, 2 a precondition missing.
set -u
cd "$(dirname "$0")/.."   # repo root: every path below is repo-relative

ROOT=""
while [ $# -gt 0 ]; do
    case "$1" in
        -*) echo "ABORT: unknown argument '$1' (usage: $0 [ROOT])" >&2; exit 2 ;;
        *)  if [ -n "$ROOT" ]; then
                echo "ABORT: more than one ROOT ('$ROOT' and '$1')" >&2; exit 2
            fi
            ROOT="$1" ;;
    esac
    shift
done
[ -n "$ROOT" ] || ROOT="$(pwd)"

ok()  { echo "ok:   $*"; }
bad() { echo "FAIL: $*"; fail=1; }
fail=0

MKF="$ROOT/Makefile"
CAP="$ROOT/kernel/cap.c"
CONS="$ROOT/kernel/console_service.c"
MMK="$ROOT/kernel/microkernel.c"
ENV="$ROOT/kernel/env_catalog.c"
TST="$ROOT/tests/env_catalog_host_test.c"
RUNALL="$ROOT/tests/run_all.sh"

# Precondition: every file the clauses read. Missing is a refusal to
# evaluate (exit 2) — not a pass, and not a finding.
for f in "$MKF" "$CAP" "$CONS" "$MMK" "$ENV" "$TST" "$RUNALL"; do
    [ -f "$f" ] || { echo "ABORT: ${f#"$ROOT"/} missing — the guard cannot be evaluated" >&2; exit 2; }
done

# has <file> <fixed string> — 0 when present. Fixed-string on purpose: every
# anchor below is source text, not a pattern.
has() { grep -qF -- "$2" "$1" 2>/dev/null; }
# hasstr <text> <fixed string> — the same, over an extracted body.
hasstr() { printf '%s\n' "$1" | grep -qF -- "$2" 2>/dev/null; }
# lines <text> <fixed string> — the line number of the first match inside an
# extracted body, or empty. The ordering half of W5.
lines() { printf '%s\n' "$1" | grep -nF -- "$2" 2>/dev/null | head -1 | cut -d: -f1; }
# fn_body <file> <literal first line> — from the line that starts with the
# pattern to the first '^}' at column 0 (the shape every fn in these files has).
fn_body() {
    awk -v pat="$2" 'index($0, pat) == 1 { grab = 1 }
                     grab { print }
                     grab && /^}/ { exit }' "$1"
}
# order_ok <file> <first-fixed-string> <second-fixed-string> — 0 when both
# are present and the first is on an earlier line. The N17b shape: an arm
# that moved out of its branch still compiles, and only its position says so.
order_ok() {
    local a b
    a=$(grep -nF -- "$2" "$1" 2>/dev/null | head -1 | cut -d: -f1)
    b=$(grep -nF -- "$3" "$1" 2>/dev/null | head -1 | cut -d: -f1)
    [ -n "$a" ] && [ -n "$b" ] && [ "$a" -lt "$b" ]
}
# clause <mark> <id> <label> — exactly one ok line when nothing under that
# clause failed; the FAIL lines above it already carry the id.
clause() { [ "$fail" -eq "$1" ] && ok "$2. $3"; }

# ── W1. the Makefile compiles the service ────────────────────────────────
# The first wire, and the only one that is invisible to everything else: a
# service that is not compiled cannot be registered, ticked or skipped, and
# no amount of correct source changes that.
c1=$fail
has "$MKF" 'kernel/env_catalog.c' ||
    bad "W1. Makefile: kernel/env_catalog.c is not compiled into the kernel — the service would not exist in a boot"
clause "$c1" W1 "kernel/env_catalog.c is compiled into the kernel"

# ── W2. cap_create_sidecar() reaches the registration ────────────────────
# §9.1 says the service is "registered by cap_create_sidecar()". The work
# is three hops down (cap_create_sidecar -> cap_create_sidecar_in ->
# cap_create_sidecar_in_personality), so the chain is pinned hop by hop as
# well as the register call: breaking any link leaves a service that exists,
# compiles, and is never wired for any environment.
c2=$fail
has "$CAP" 'int cap_create_sidecar(uint32_t parent_pid,' ||
    bad "W2. cap.c: cap_create_sidecar() is gone — there is nothing to reach the registration from"
has "$CAP" 'return cap_create_sidecar_in(parent_pid, manifest, manifest_len,' ||
    bad "W2. cap.c: cap_create_sidecar() no longer delegates to cap_create_sidecar_in() — the chain to the registration is broken"
has "$CAP" 'return cap_create_sidecar_in_personality(parent_pid, manifest, manifest_len,' ||
    bad "W2. cap.c: cap_create_sidecar_in() no longer calls cap_create_sidecar_in_personality(), the function the arm lives in"
has "$CAP" 'env_catalog_service_register(k_rd, k_wr,' ||
    bad "W2. cap.c: the catalog arm no longer registers the channel with the service — an unreachable service no environment could use"
clause "$c2" W2 "cap_create_sidecar() reaches the registration, hop by hop"

# ── W3. the arm is gated on the kernel-owned name ────────────────────────
# "kernel.env.catalog" is kernel-owned, which is the half of the boundary a
# manifest cannot reach: the peer is resolved by the "kernel." branch, never
# by sidecar_registry_resolve(), so no manifest can name another tenant's
# catalog. The ORDER teeth pin the arm where that is true — after the gate
# opens, before the tenant-resolvable fallback — because an arm moved out of
# the branch still compiles and still registers.
c3=$fail
has "$CAP" 'if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {' ||
    bad "W3. cap.c: the catalog arm is no longer gated on the kernel-owned name kernel.env.catalog"
order_ok "$CAP" 'if (sidecar_prefix(sc->peer_name, "kernel.")) {' \
                'if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {' ||
    bad "W3. cap.c: the catalog arm must sit AFTER the kernel-owned branch opens — outside it, a tenant-resolvable peer could be registered as a catalog"
order_ok "$CAP" 'if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {' \
                'uint32_t peer_pid = sidecar_registry_resolve(sc->peer_name,' ||
    bad "W3. cap.c: the catalog arm must sit BEFORE the registry-resolve fallback — a catalog peer resolved through the tenant registry would be a cross-partition channel by name"
has "$ENV" 'refused by kernel.env.catalog' ||
    bad "W3. env_catalog.c: the refusals no longer name the service they come from (the by-name text the guard and the live clause grep)"
clause "$c3" W3 "the arm is gated on kernel.env.catalog, between the kernel-owned gate and the registry fallback, and answers under that name"

# ── W4. console_service_tick() skips the channel's kernel slot ───────────
# CAT_* frames are not console text. Printing them puts a tenant's OBJECT
# NAMES on the shared serial transcript — the leak E6 exists to prevent —
# and, before that, consumes the frame env_catalog_service_tick() must
# answer, so the verb simply never happens. Third exclusion of the shape
# env_console and net_socket already established.
c4=$fail
has "$CONS" '#include "env_catalog.h"' ||
    bad "W4. console_service.c: env_catalog.h is not included — the skip cannot be expressed"
CONS_BODY="$(fn_body "$CONS" 'void console_service_tick(void) {')"
if ! hasstr "$CONS_BODY" 'if (env_catalog_service_kernel_slot((uint16_t)s)) continue;'; then
    bad "W4. console_service_tick() must skip the catalog channel's kernel slot with a continue — CAT_* frames must never reach the shared serial transcript"
fi
clause "$c4" W4 "console_service_tick() skips the catalog channel's kernel slot"

# ── W5. the tick runs in microkernel_service_poll(), beside the others ───
# The service holds no lock of its own because it has exactly one writer:
# this non-IRQ context. A tick called from anywhere else (the timer ISR, a
# syscall) would be a second writer, and a tick left out entirely means a
# registered channel that is never drained — the handshake never answered.
c5=$fail
has "$MMK" '#include "env_catalog.h"' ||
    bad "W5. microkernel.c: env_catalog.h is not included"
POLL="$(fn_body "$MMK" 'void microkernel_service_poll(void) {')"
if ! hasstr "$POLL" 'env_catalog_service_tick();'; then
    bad "W5. microkernel_service_poll() never ticks the catalog service — registered channels would never be drained"
fi
if ! hasstr "$POLL" 'net_socket_service_tick();'; then
    bad "W5. the sibling tick (net_socket_service_tick) is gone from the same poll — the 'beside' half of §9.1's sentence can no longer be true"
fi
a="$(lines "$POLL" 'net_socket_service_tick();')"
b="$(lines "$POLL" 'env_catalog_service_tick();')"
if [ -n "$a" ] && [ -n "$b" ] && [ "$a" -ge "$b" ]; then
    bad "W5. env_catalog_service_tick() must run after net_socket_service_tick() in the same poll (the order §9.1 names)"
fi
clause "$c5" W5 "env_catalog_service_tick() runs in microkernel_service_poll() after net_socket_service_tick()"

# ── B1. THE BOUNDARY: filtered on the registered partition, as absence ───
# The property §9.1 exists to establish, in the two places that enforce it
# and the one that would let the wire move it:
#   - the lookup and the list walk both SKIP a foreign entry (continue, not
#     a refusal), and the count that becomes CAT_LIST's `total` counts with
#     the same predicate — a total that disagreed with the walk would leak
#     how many names a tenant may not see;
#   - the partition comes from the register argument, stored exactly once
#     as a live value (the only other store clears it on release), and
#     every filter reads e->partition — so there is no verb, no frame field
#     and no length a caller could supply to move it;
#   - a foreign name is answered CAT_ERR_NOENT — the SAME refusal a name
#     that never existed gets — while PERM stays reserved for a name that IS
#     in this namespace and is still not served. Two answers, two meanings.
#     A distinct PERM for a foreign name would turn a list call into an
#     existence oracle across the LPAR boundary, which is the whole point.
c6=$fail
has "$ENV" 'if (o->partition_id != partition) continue;' ||
    bad "B1. env_catalog.c: cat_find() no longer skips a foreign-partition entry — a lookup could return another tenant's object"
has "$ENV" 'if (o->partition_id != e->partition) continue;' ||
    bad "B1. env_catalog.c: the CAT_LIST walk no longer skips foreign entries — a foreign NAME would appear in a tenant's listing"
has "$ENV" 'if (o->active && o->partition_id == partition) c++;' ||
    bad "B1. env_catalog.c: cat_partition_count() no longer counts with the same predicate — CAT_LIST's total would disagree with its own walk"
has "$ENV" 'e->partition = partition;' ||
    bad "B1. env_catalog.c: the register argument is no longer what stores the partition — the boundary would have a second source"
NSTORES="$(grep -cF 'e->partition = ' "$ENV" 2>/dev/null)"
if [ "$NSTORES" != "2" ]; then
    bad "B1. env_catalog.c: expected exactly two stores of e->partition (register, and release clearing it), found $NSTORES — a verb that assigned the partition from the frame would move the boundary from the wire"
fi
# Every filter reads the REGISTERED partition: a cat_find()/cat_partition_
# count() called with anything else (cat_buf, a parsed field) is the bug.
for fn in cat_find cat_partition_count; do
    # Definition lines are `static …`; every other mention is a call site or
    # a comment, and every call site must read e->partition.
    stray="$(grep -nF "$fn(" "$ENV" 2>/dev/null | grep -vF 'static ' | grep -vF 'e->partition')"
    if [ -n "$stray" ]; then
        bad "B1. env_catalog.c: $fn() is called with something other than the registered partition — $(printf '%s' "$stray" | head -1)"
    fi
done
ARM="$(grep -A6 -F 'sidecar_prefix(sc->peer_name, "kernel.env.catalog")' "$CAP" 2>/dev/null)"
if ! hasstr "$ARM" 'pd->pid, pd->name'; then
    bad "B1. cap.c: the registration no longer carries the sidecar's pid and name — the refusal could not name its caller"
fi
if ! hasstr "$ARM" 'pd->partition_id,'; then
    bad "B1. cap.c: the catalog arm must read the partition off the process descriptor, not a caller-supplied field"
fi
has "$ENV" 'cat_refuse(e, ty, tag, CAT_ERR_NOENT, 0)' ||
    bad "B1. env_catalog.c: a foreign name is no longer answered CAT_ERR_NOENT — a distinct PERM would confirm the name exists somewhere the caller cannot see"
has "$ENV" 'cat_refuse(e, ty, tag, CAT_ERR_PERM, "SYSTEM_METADATA is not tenant data")' ||
    bad "B1. env_catalog.c: the in-namespace PERM refusal is gone — NOENT and PERM must remain DIFFERENT answers, or both mean nothing"
clause "$c6" B1 "the boundary: filtered on the registered partition as absence, sourced from pd->partition_id, unmovable by the wire"

# ── B2. the boundary's evidence: the host test, and that it is RUN ───────
# §9.1 names the host test as the thing that establishes the property. Three
# things must hold for that to be true: it links the REAL service (not a
# reimplementation), it pins the clause that makes the boundary a security
# property rather than a bug report, and run_all.sh's glob actually reaches
# it — a test with no extractable 'gcc' line is SKIPped by name, silently,
# and the boundary would then be guarded by nothing at all.
c7=$fail
has "$TST" 'kernel/env_catalog.c' ||
    bad "B2. the host test must link the REAL kernel/env_catalog.c, not a reimplementation of the service under test"
has "$TST" 'THE TWO ANSWERS ARE INDISTINGUISHABLE' ||
    bad "B2. the host test no longer pins the indistinguishable-NOENT clause — the property §9.1 exists to establish would be unasserted"
has "$TST" 'THE BOUNDARY:' ||
    bad "B2. the host test no longer carries a named BOUNDARY clause"
if ! grep -qE '\* +gcc ' "$TST" 2>/dev/null; then
    bad "B2. the host test has no '* gcc …' build line for run_all.sh to extract — the runner would SKIP it with no failure anywhere"
fi
has "$RUNALL" 'for src in tests/*_host_test.c; do' ||
    bad "B2. run_all.sh no longer globs tests/*_host_test.c — the host suite would never build this test"
clause "$c7" B2 "the host test links the real service, pins the indistinguishable-NOENT clause, and is built by run_all.sh"

echo ""
if [ "$fail" -eq 0 ]; then
    echo "env_catalog_broker_check: all clauses held"
else
    echo "env_catalog_broker_check: FAILED"
fi
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)
