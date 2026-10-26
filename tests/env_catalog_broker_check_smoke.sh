#!/usr/bin/env bash
# tests/env_catalog_broker_check_smoke.sh — teeth for tests/env_catalog_broker_check.sh.
#
# The guard's own header says this file was NOT written when the teeth were:
#
#     "A committed smoke that reddens each tooth below when the property is
#      removed is NOT written yet — said here rather than implied, so that
#      nobody reads the teeth above as proven when only the green run has
#      been."
#
# This is that file. A guard nobody has watched fail is a guard nobody
# trusts, so every clause gets at least one arm that deletes or distorts
# exactly the thing it names and requires the guard to exit 1 SAYING WHICH.
# Each mutation is a mistake a future edit could plausibly make — a store
# "cleaned up" as dead code, an arm moved out of its branch, a predicate
# "simplified" — not an artificial corruption; the point is that the guard,
# not the author, catches it.
#
# The clause set, and what reddens it:
#   W1. the Makefile line that compiles the service;
#   W2. the four-hop chain cap_create_sidecar -> ..._in -> ..._in_personality
#       -> env_catalog_service_register();
#   W3. the gate on the kernel-owned name, its two ORDER teeth (the arm
#       must sit after the kernel. branch opens and before the registry
#       fallback) and the by-name refusal text;
#   W4. the include and the skip in console_service_tick();
#   W5. the include, the tick, and its order after net_socket_service_tick();
#   B1. the four filters, the two-store rule, the strays rule, the two
#       cap.c arm fields, and the NOENT/PERM distinction;
#   B2. the host test's link, its two named clauses, its gcc build line,
#       and run_all.sh's glob.
#
# Mutations run on COPIES (seed() below): the real tree is never touched,
# and the baseline run on an unmutated copy — plus the final run on the
# real tree — is the seed-fidelity check. If a copy could not carry the
# wiring faithfully, the mutated runs would fail to fail and this file
# would say so.
#
# A smoke that cannot run proves nothing, so every failure here is a
# failure: non-zero exit, or a mutation the guard did not name.
#
# GUARD-KIND: host (plain bash over copies of the tree; no build, no QEMU).
#
# Run:  bash tests/env_catalog_broker_check_smoke.sh
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2

GUARD=tests/env_catalog_broker_check.sh
# Exactly the files the guard reads (its own precondition loop names them).
FILES=(
    Makefile
    kernel/cap.c
    kernel/console_service.c
    kernel/microkernel.c
    kernel/env_catalog.c
    tests/env_catalog_host_test.c
    tests/run_all.sh
)

for f in "${FILES[@]}" "$GUARD"; do
    [ -f "$f" ] || { echo "ABORT: $f missing." >&2; exit 2; }
done

W="$(mktemp -d)" || { echo "ABORT: mktemp failed." >&2; exit 2; }
cleanup() { rm -rf "$W"; return 0; }
trap cleanup EXIT

seed() {
    local d="$1" f
    rm -rf "$d"
    for f in "${FILES[@]}"; do
        mkdir -p "$d/$(dirname "$f")"
        cp "$f" "$d/$f" || return 1
    done
    return 0
}

# The guard takes ROOT as its single argument (see its own usage line).
run_guard() { bash "$GUARD" "$1" 2>&1; }

pass=0
fail=0

# tooth <name> <clause> <description> <dir> — the guard must exit 1 and the
# output must name <clause>. Anything else is a smoke failure: a guard that
# stays green under its own mutation is the vacuous pass this file exists
# to refuse.
tooth() {
    local name="$1" clause="$2" desc="$3" d="$4" out rc
    out="$(run_guard "$d")"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "FAIL: $clause\."; then
        echo "TOOTH OK   $name — guard exited 1 naming $clause"
        pass=$((pass+1))
    else
        echo "TOOTH FAIL $name — guard rc=$rc (want 1), clause '$clause' $(printf '%s\n' "$out" | grep -c "FAIL: $clause\.") time(s)"
        printf '%s\n' "$out" | grep "FAIL:" | head -4 | sed 's/^/           /'
        fail=$((fail+1))
    fi
}

echo "env_catalog_broker_check_smoke — teeth for §9.1's five wires and the"
echo "boundary they exist to carry:"
echo "  W1. the Makefile compiles the service"
echo "  W2. cap_create_sidecar() reaches the registration, hop by hop"
echo "  W3. the arm is gated on kernel.env.catalog, in the right place, and answers under it"
echo "  W4. console_service_tick() skips the catalog channel's kernel slot"
echo "  W5. the tick runs in microkernel_service_poll(), after net_socket_service_tick()"
echo "  B1. the boundary: filtered on the registered partition, as absence, unmovable by the wire"
echo "  B2. the boundary's evidence: the host test links the real service and is RUN"
echo "========================================================================"
echo

# ─── Baseline: the unmutated copy passes (vacuity control) ────────────────
seed "$W/base"
if out="$(run_guard "$W/base")" && [ $? -eq 0 ]; then
    echo "ok:   baseline — the seeded copy passes the guard unmutated"
else
    echo "ABORT: baseline — the seeded copy does not pass the guard unmutated:"
    printf '%s\n' "$out" | grep "FAIL:" | head -5 | sed 's/^/       /'
    exit 2
fi
echo

# ─── W1. the service is not compiled ──────────────────────────────────────
# A service that is not compiled cannot be registered, ticked or skipped.
seed "$W/t1"
sed -i '/kernel\/env_catalog\.c \\/d' "$W/t1/Makefile"
tooth makefile-line-gone W1 "kernel/env_catalog.c is dropped from X86_C_SRC" "$W/t1"

# ─── W2. the chain to the registration, hop by hop ────────────────────────
seed "$W/t2"
sed -i 's/int cap_create_sidecar(uint32_t parent_pid,/int cap_create_sidecar(uint32_t pp,/' "$W/t2/kernel/cap.c"
tooth entry-renamed W2 "the entry point the manifests call no longer has its name" "$W/t2"

seed "$W/t3"
sed -i 's/return cap_create_sidecar_in(parent_pid, manifest, manifest_len,/return cap_create_sidecar_in(parent_pid, manifest, mlen,/' "$W/t3/kernel/cap.c"
tooth hop1-broken W2 "cap_create_sidecar() no longer delegates to cap_create_sidecar_in()" "$W/t3"

seed "$W/t4"
sed -i 's/return cap_create_sidecar_in_personality(parent_pid, manifest, manifest_len,/return cap_create_sidecar_in_personality(parent_pid, manifest, mlen,/' "$W/t4/kernel/cap.c"
tooth hop2-broken W2 "the delegation no longer reaches cap_create_sidecar_in_personality()" "$W/t4"

seed "$W/t5"
sed -i 's/if (!env_catalog_service_register(k_rd, k_wr,/if (!env_catalog_service_register(k_rd, kw,/' "$W/t5/kernel/cap.c"
tooth register-call-gone W2 "the arm no longer registers the channel with the service" "$W/t5"

# ─── W3. the gate on the kernel-owned name, and its two ORDER teeth ───────
seed "$W/t6"
sed -i '/if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {/d' "$W/t6/kernel/cap.c"
tooth gate-removed W3 "the arm is no longer gated on kernel.env.catalog" "$W/t6"

# The arm moved OUT of the kernel. branch — the mutation the order teeth
# exist for: it still compiles, and only its position says so. Implemented
# as a genuine line move rather than a deletion, because a deletion tests
# the first anchor and not the ordering.
seed "$W/t7"
python3 - "$W/t7/kernel/cap.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding='utf-8').read().split('\n')
gate = '        if (sidecar_prefix(sc->peer_name, "kernel.")) {'
arm  = '                if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {'
i, j = lines.index(gate), lines.index(arm)
assert i < j, "expected the kernel. branch to open before the catalog arm"
moved = lines.pop(i)
# index shifted by one only if the removed line was before the insertion
j = lines.index(arm)
lines.insert(j + 1, moved)
open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
PY
tooth arm-left-branch W3 "the catalog arm now sits AFTER the kernel-owned branch opens" "$W/t7"

# ...and the other direction: the tenant-resolvable registry fallback
# dragged up in front of the arm, so the arm would be reachable only after
# a cross-partition resolve.
seed "$W/t8"
python3 - "$W/t8/kernel/cap.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding='utf-8').read().split('\n')
res  = '            uint32_t peer_pid = sidecar_registry_resolve(sc->peer_name,'
arm  = '                if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {'
i, j = lines.index(res), lines.index(arm)
assert i > j, "expected the registry fallback after the catalog arm"
moved = lines.pop(i)
j = lines.index(arm)
lines.insert(j, moved)
open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
PY
tooth arm-after-fallback W3 "the registry-resolve fallback now precedes the catalog arm" "$W/t8"

seed "$W/t9"
sed -i 's/ refused by kernel.env.catalog/ refused by the catalog service/' "$W/t9/kernel/env_catalog.c"
tooth refusal-unbranded W3 "the service's refusals no longer name the service" "$W/t9"

# ─── W4. console_service_tick() must skip the kernel slot ─────────────────
seed "$W/t10"
sed -i '/#include "env_catalog.h"/d' "$W/t10/kernel/console_service.c"
tooth console-include-gone W4 "console_service.c no longer includes env_catalog.h" "$W/t10"

seed "$W/t11"
sed -i '/if (env_catalog_service_kernel_slot((uint16_t)s)) continue;/d' "$W/t11/kernel/console_service.c"
tooth console-skip-gone W4 "CAT_* frames would be printed to the shared serial transcript" "$W/t11"

# ─── W5. the tick in microkernel_service_poll(), in order ─────────────────
seed "$W/t12"
sed -i '/#include "env_catalog.h"/d' "$W/t12/kernel/microkernel.c"
tooth poll-include-gone W5 "microkernel.c no longer includes env_catalog.h" "$W/t12"

seed "$W/t13"
sed -i '/^    env_catalog_service_tick();$/d' "$W/t13/kernel/microkernel.c"
tooth poll-tick-gone W5 "registered channels would never be drained" "$W/t13"

seed "$W/t14"
python3 - "$W/t14/kernel/microkernel.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding='utf-8').read().split('\n')
net = '    net_socket_service_tick();'
cat = '    env_catalog_service_tick();'
i, j = lines.index(net), lines.index(cat)
assert i < j, "expected net_socket_service_tick() before env_catalog_service_tick()"
lines[i], lines[j] = lines[j], lines[i]
open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
PY
tooth tick-order-flipped W5 "the catalog tick now runs BEFORE net_socket_service_tick()" "$W/t14"

# ─── B1. THE BOUNDARY ─────────────────────────────────────────────────────
seed "$W/t15"
sed -i '/if (o->partition_id != partition) continue;/d' "$W/t15/kernel/env_catalog.c"
tooth lookup-filter-gone B1 "cat_find() could return another tenant's object" "$W/t15"

seed "$W/t16"
sed -i '/if (o->partition_id != e->partition) continue;/d' "$W/t16/kernel/env_catalog.c"
tooth list-filter-gone B1 "a foreign NAME would appear in a tenant's listing" "$W/t16"

seed "$W/t17"
sed -i 's/if (o->active \&\& o->partition_id == partition) c++;/if (o->active) c++;/' "$W/t17/kernel/env_catalog.c"
tooth count-predicate-loosened B1 "CAT_LIST's total would disagree with its own walk" "$W/t17"

seed "$W/t18"
sed -i 's/    e->partition = partition;/    e->partition = 0;/' "$W/t18/kernel/env_catalog.c"
tooth partition-store-gone B1 "the register argument no longer stores the partition" "$W/t18"

seed "$W/t19"
python3 - "$W/t19/kernel/cap.c" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, encoding='utf-8').read().split('\n')
arm = '                if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {'
i = lines.index(arm)
hits = 0
for k in range(i, min(i + 7, len(lines))):
    if 'pd->partition_id,' in lines[k]:
        lines[k] = lines[k].replace('pd->partition_id,', 'partition_from_wire,')
        hits += 1
assert hits == 1, f"expected exactly one pd->partition_id in the arm, found {hits}"
open(p, 'w', encoding='utf-8', newline='').write('\n'.join(lines))
PY
tooth partition-not-from-pd B1 "the arm no longer reads the partition off the process descriptor" "$W/t19"

seed "$W/t20"
sed -i 's/cat_refuse(e, ty, tag, CAT_ERR_NOENT, 0)/cat_refuse(e, ty, tag, CAT_ERR_PERM, 0)/' "$W/t20/kernel/env_catalog.c"
tooth foreign-answered-perm B1 "a foreign name would be answered PERM — an existence oracle" "$W/t20"

seed "$W/t21"
sed -i 's/CAT_ERR_PERM, "SYSTEM_METADATA is not tenant data"/CAT_ERR_PERM, "no read"/' "$W/t21/kernel/env_catalog.c"
tooth perm-refusal-gone B1 "NOENT and PERM would no longer be DIFFERENT answers" "$W/t21"

seed "$W/t22"
sed -i 's/cat_partition_count(e->partition)/cat_partition_count(the_frame_field)/' "$W/t22/kernel/env_catalog.c"
tooth count-called-off-register B1 "cat_partition_count() is called with something other than e->partition" "$W/t22"

# ─── B2. the boundary's evidence: the host test, and that it is RUN ───────
seed "$W/t23"
sed -i 's|kernel/env_catalog.c|kernel/env_catalog_reimplementation.c|g' "$W/t23/tests/env_catalog_host_test.c"
tooth test-not-real-service B2 "the host test no longer links the real service" "$W/t23"

seed "$W/t24"
sed -i 's/THE TWO ANSWERS ARE INDISTINGUISHABLE/THE TWO ANSWERS ARE DIFFERENT/' "$W/t24/tests/env_catalog_host_test.c"
tooth indistinguishable-clause-gone B2 "the property §9.1 rests on is unasserted" "$W/t24"

seed "$W/t25"
sed -i 's/THE BOUNDARY:/THE BORDER:/g' "$W/t25/tests/env_catalog_host_test.c"
tooth boundary-clause-gone B2 "the host test carries no named BOUNDARY clause" "$W/t25"

seed "$W/t26"
sed -i '/gcc -no-pie -Wall -Wextra -std=c11/d' "$W/t26/tests/env_catalog_host_test.c"
tooth no-extractable-build-line B2 "run_all.sh would SKIP the test with no failure anywhere" "$W/t26"

seed "$W/t27"
sed -i 's|for src in tests/\*_host_test.c; do|for src in tests/*_host_test_never.c; do|' "$W/t27/tests/run_all.sh"
tooth runner-glob-gone B2 "run_all.sh would never build the host test" "$W/t27"

echo
echo "────────────────────────────────────────────────────────────────────────"
echo "tooth baseline on the REAL tree (seed fidelity, second control):"
out="$(run_guard "$(pwd)")"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   baseline — the real tree passes the guard"
    pass=$((pass+1))
else
    echo "TOOTH FAIL baseline — the real tree FAILED the guard (rc=$rc):"
    printf '%s\n' "$out" | grep "FAIL:" | head -5 | sed 's/^/           /'
    fail=$((fail+1))
fi

echo
if [ "$fail" -eq 0 ]; then
    echo "env_catalog_broker_check_smoke: $pass teeth held, 0 lost"
    exit 0
fi
echo "env_catalog_broker_check_smoke: $pass held, $fail LOST"
exit 1
