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
#       and run_all.sh's glob;
#   C1. the cap: the tenant manifest's catalog peer, its rights, the count,
#       the SYSTEM profile's ABSENCE of it, and its pin test;
#   C2. the client: the CAT magic, the four trait verbs, the status-word
#       read, the surfaced refusal text, and the three wiring hops that
#       reach a shell command (BIB resolve, install, registration).
#   Q1. CAT_QUERY on both halves: the kernel's verb table and header,
#       proto's two constants, the encoder/decoder, the trait method,
#       the client's call, and the `sql` subcommand that reaches it;
#   Q2. the ORDER of the QUERY arm's gates — the policy check and the
#       boundary lookup each deleted once, and the parse/lookup pair
#       genuinely SWAPPED (the mutation order teeth exist for: a gate
#       moved still compiles and still looks like a gate);
#   Q3. read-only posture: a sixth verb, a second execution site, the
#       unknown-verb refusal, and the send length (g.off -> off).
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
    kernel/env_catalog.h
    tests/env_catalog_host_test.c
    tests/run_all.sh
    # C1/C2: the tenant half — the manifest that asks for the channel and
    # the client that speaks on it.
    user/init/src/posix_manifest.rs
    user/proto/src/lib.rs
    user/proto/src/catalops.rs
    user/sidecar/src/catalog_client.rs
    user/sidecar/src/boot.rs
    user/sidecar/src/entry.rs
    user/sidecar/src/applets.rs
    user/procmgr/src/procmgr.rs
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

echo "env_catalog_broker_check_smoke — teeth for §9.1's five wires, its"
echo "boundary, and the tenant half (cap + client) that makes it reachable:"
echo "  W1. the Makefile compiles the service"
echo "  W2. cap_create_sidecar() reaches the registration, hop by hop"
echo "  W3. the arm is gated on kernel.env.catalog, in the right place, and answers under it"
echo "  W4. console_service_tick() skips the catalog channel's kernel slot"
echo "  W5. the tick runs in microkernel_service_poll(), after net_socket_service_tick()"
echo "  B1. the boundary: filtered on the registered partition, as absence, unmovable by the wire"
echo "  B2. the boundary's evidence: the host test links the real service and is RUN"
echo "  C1. the cap: the tenant manifest names kernel.env.catalog (and the system one does not)"
echo "  C2. the client: four verbs, the status word, the refusal text, and the wiring to a command"
echo "  Q1. CAT_QUERY on both halves, with matching numbers and a reachable sql subcommand"
echo "  Q2. the QUERY arm's gates in order (parse -> lookup -> uid -> policy -> execute)"
echo "  Q3. read-only: five verbs, one execution site, and the reply sent at g.off"
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
awk -v gate='        if (sidecar_prefix(sc->peer_name, "kernel.")) {' \
    -v arm='                if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {' '
    $0 == gate { held = 1; next }
    { print }
    $0 == arm && held { print gate; held = 0 }
    END { if (held) print gate }
' "$W/t7/kernel/cap.c" > "$W/t7/kernel/cap.c.new"
mv "$W/t7/kernel/cap.c.new" "$W/t7/kernel/cap.c"
tooth arm-left-branch W3 "the catalog arm now sits AFTER the kernel-owned branch opens" "$W/t7"

# ...and the other direction: the tenant-resolvable registry fallback
# dragged up in front of the arm, so the arm would be reachable only after
# a cross-partition resolve.
seed "$W/t8"
# A two-pass awk: the line being moved comes AFTER its destination in the
# file, so the whole file is read before it is re-emitted without it.
awk -v arm='                if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {' \
    -v res='            uint32_t peer_pid = sidecar_registry_resolve(sc->peer_name,' '
    { line[NR] = $0 }
    END {
        done = 0
        for (i = 1; i <= NR; i++) {
            if (line[i] == res) continue
            if (line[i] == arm && !done) { print res; done = 1 }
            print line[i]
        }
        if (!done) print res
    }
' "$W/t8/kernel/cap.c" > "$W/t8/kernel/cap.c.new"
mv "$W/t8/kernel/cap.c.new" "$W/t8/kernel/cap.c"
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
awk -v net='    net_socket_service_tick();' -v cat='    env_catalog_service_tick();' '
    $0 == net && !sw { print cat; sw = 1; next }
    $0 == cat && sw && !done { print net; done = 1; next }
    { print }
' "$W/t14/kernel/microkernel.c" > "$W/t14/kernel/microkernel.c.new"
mv "$W/t14/kernel/microkernel.c.new" "$W/t14/kernel/microkernel.c"
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
# Scoped, like the python this replaces: only the six lines the guard's ARM
# extraction sees may change — a global s/// would mutate every other
# pd->partition_id in the file and redden for the wrong reason.
awk -v arm='                if (sidecar_prefix(sc->peer_name, "kernel.env.catalog")) {' '
    { line[NR] = $0 }
    END {
        done = 0
        for (i = 1; i <= NR; i++) {
            if (!done && line[i] == arm) {
                for (j = i + 1; j <= i + 6 && j <= NR; j++) {
                    if (!done && index(line[j], "pd->partition_id,")) {
                        sub(/pd->partition_id,/, "partition_from_wire,", line[j])
                        done = 1
                    }
                }
            }
            print line[i]
        }
        if (!done) exit 1
    }
' "$W/t19/kernel/cap.c" > "$W/t19/kernel/cap.c.new"
mv "$W/t19/kernel/cap.c.new" "$W/t19/kernel/cap.c"
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

# ─── C1. the cap a manifest must name ─────────────────────────────────────
seed "$W/t28"
sed -i 's/peer: Some("kernel.env.catalog"),/peer: Some("drv.catalog.0"),/' \
    "$W/t28/user/init/src/posix_manifest.rs"
tooth tenant-peer-rewritten C1 "the tenant's catalog cap no longer peers at the kernel-owned service" "$W/t28"

seed "$W/t29"
sed -i 's/n_caps: 5,/n_caps: 4,/' "$W/t29/user/init/src/posix_manifest.rs"
tooth cap-not-counted C1 "n_caps stops counting the catalog cap" "$W/t29"

seed "$W/t30"
sed -i 's/rights: 0x7, \/\/ R | W | send — CAT_\*/rights: 0x3, \/\/ R | W | send — CAT_*/' \
    "$W/t30/user/init/src/posix_manifest.rs"
tooth catalog-rights-shrunk C1 "the catalog cap's rights shrink below its siblings" "$W/t30"

# "give the system profile the same cap" — the plausible edit the
# ABSENCE anchor exists for: only the tenant profile was to change, and a
# shared cap would put a second, differently-partitioned channel behind
# the same kernel-owned name.
seed "$W/t31"
sed -i '/        n_caps: if e1000_bar0_phys.is_some() { 8 } else { 7 },/i\        peer: Some("kernel.env.catalog"), // same as the tenant profile' \
    "$W/t31/user/init/src/posix_manifest.rs"
tooth system-gains-catalog C1 "the SYSTEM profile was given a catalog cap too" "$W/t31"

seed "$W/t32"
sed -i 's/assert_eq!(cat_peer, "kernel.env.catalog",/assert_eq!(cat_peer, "drv.catalog.0",/' \
    "$W/t32/user/init/src/posix_manifest.rs"
tooth pin-peer-rotated C1 "the pin test no longer asserts the kernel-owned catalog peer" "$W/t32"

# ─── C2. the client, its four verbs and its status discipline ─────────────
seed "$W/t33"
sed -i 's/pub const CAT_MAGIC: \[u8; 8\] = \*b"AEROSCT/pub const CAT_MAGIC: [u8; 8] = *b"AEROSNT/' \
    "$W/t33/user/proto/src/lib.rs"
tooth magic-collision C2 "the client's CAT magic becomes the NET magic — the two services parse each other" "$W/t33"

seed "$W/t34"
sed -i '/fn cat_read(&mut self/d' "$W/t34/user/proto/src/catalops.rs"
tooth verb-dropped C2 "the trait loses CAT_READ — three verbs where §9.1 names four" "$W/t34"

seed "$W/t35"
sed -i 's/let status = cat_status(reply)/let status = CAT_OK/' "$W/t35/user/sidecar/src/catalog_client.rs"
tooth status-skipped C2 "the client stops reading the status word off every reply" "$W/t35"

seed "$W/t36"
sed -i 's/cat_text(reply)/None/' "$W/t36/user/sidecar/src/catalog_client.rs"
tooth refusal-text-dropped C2 "the service's rendered refusal never reaches the caller's transcript" "$W/t36"

seed "$W/t37"
sed -i 's/bib.find_cap(CAP_CHAN, "catalog")/bib.find_cap(CAP_CHAN, "console")/' "$W/t37/user/sidecar/src/boot.rs"
tooth bib-cap-unresolved C2 "the granted catalog cap is never resolved into a client" "$W/t37"

seed "$W/t38"
sed -i '/booted.proc.set_catalog(/d' "$W/t38/user/sidecar/src/entry.rs"
tooth client-not-installed C2 "the client is constructed but never installed into the ProcManager" "$W/t38"

seed "$W/t39"
sed -i '/pm.register_applet("catalog", catalog);/d' "$W/t39/user/sidecar/src/applets.rs"
tooth applet-unregistered C2 "the surface has no command — nobody can reach it" "$W/t39"

# ─── Q1. CAT_QUERY exists on BOTH halves with the same numbers ────────────
seed "$W/t40"
sed -i 's/pub const CAT_QUERY: u16 = 5;/pub const CAT_QUERY: u16 = 6;/' \
    "$W/t40/user/proto/src/lib.rs"
tooth query-number-mismatch Q1 "proto's CAT_QUERY no longer carries the kernel's number 5" "$W/t40"

seed "$W/t41"
sed -i '/    { 5, "CAT_QUERY" },/d' "$W/t41/kernel/env_catalog.c"
tooth query-not-in-table Q1 "the kernel's verb table no longer names CAT_QUERY" "$W/t41"

seed "$W/t42"
sed -i '/"sql" => {/d' "$W/t42/user/sidecar/src/applets.rs"
tooth sql-subcommand-gone Q1 "the SQL path has no user in a shell — verb 5 is unreachable" "$W/t42"

seed "$W/t43"
sed -i '/fn cat_query(&mut self/d' "$W/t43/user/proto/src/catalops.rs"
tooth query-trait-method-gone Q1 "the trait cannot reach the SQL path — a kernel verb no client can send" "$W/t43"

# ─── Q2. the ORDER of the QUERY arm's gates ────────────────────────────────
seed "$W/t44"
sed -i '/if (!catalog_check_access(uid, s->table_name, PERM_READ)) {/d' \
    "$W/t44/kernel/env_catalog.c"
tooth policy-check-gone Q2 "a denial would become an EMPTY result — denied and empty are one answer" "$W/t44"

seed "$W/t45"
sed -i '/same_partition = (proc_table\[i\].partition_id == e->partition);/d' "$W/t45/kernel/env_catalog.c"
tooth pid-partition-check-gone Q2 "a descriptor whose process moved partitions could read" "$W/t45"

# The mutation the Q2 ORDER tooth exists for: the parse call MOVED after
# the boundary lookup. It still compiles, every anchor is still present,
# and only the line positions say a name was resolved before the statement
# had parsed. Two held lines (the call wraps) re-emitted after cat_find.
seed "$W/t46"
awk '
    !held && index($0, "if (sql_parse((const char*)cat_sql, &cat_stmt, perr,") == 9 {
        p1 = $0; getline p2; held = 1; next
    }
    held && index($0, "int idx = cat_find(") {
        print; print p1; print p2; held = 0; next
    }
    { print }
    END { if (held) { print p1; print p2 } }
' "$W/t46/kernel/env_catalog.c" > "$W/t46/kernel/env_catalog.c.new"
mv "$W/t46/kernel/env_catalog.c.new" "$W/t46/kernel/env_catalog.c"
tooth parse-after-lookup Q2 "the statement now parses AFTER a name is resolved" "$W/t46"

# ─── Q3. still read-only, and the reply is the cursor's own length ─────────
seed "$W/t47"
sed -i 's/cat_send(e, cat_reply, g.off, tag);/cat_send(e, cat_reply, off, tag);/' \
    "$W/t47/kernel/env_catalog.c"
tooth send-length-rewound Q3 "the reply would promise rows its bytes do not carry" "$W/t47"

seed "$W/t48"
sed -i '/    { 5, "CAT_QUERY" },/a\    { 6, "CAT_WRITE" },' "$W/t48/kernel/env_catalog.c"
tooth sixth-verb-added Q3 "the table grew a verb §9.1 did not answer for" "$W/t48"

seed "$W/t49"
sed -i '/sql_execute(uid, (const char\*)cat_sql, &cat_result)/a\        sql_execute(uid, retry_sql, &cat_result); /* plausible re-run */' \
    "$W/t49/kernel/env_catalog.c"
tooth second-execution-site Q3 "a statement could run through a path the gate above never admitted" "$W/t49"

seed "$W/t50"
sed -i '/cat_refuse(e, ty, tag, CAT_ERR_UNKNOWN, 0);/d' "$W/t50/kernel/env_catalog.c"
tooth unknown-refusal-gone Q3 "a verb outside the table would be answered something else" "$W/t50"

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
