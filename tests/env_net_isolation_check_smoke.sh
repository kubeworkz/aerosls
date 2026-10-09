#!/usr/bin/env bash
# tests/env_net_isolation_check_smoke.sh — the TEETH for
# tests/env_net_isolation_check.sh, and the tooth v0.2 §6 names for P2's
# first increment:
#
#   P2_TOOTH=system-peer     — the tenant's `network` cap is pointed at
#                              drv.network.0 instead of the kernel service.
#                              The guard must go red at the MANIFEST pin: the
#                              peer is exactly kernel.net.socket, and a tenant
#                              pointed at the system partition's sidecar is
#                              the boundary violation this whole increment
#                              exists to withhold (at boot the same mutation
#                              refuses channel creation in E2's scoped
#                              registry, before any socket opens — the source
#                              half is what proves without a QEMU run).
#   P2_TOOTH=unattributed    — the CONNECT stops attributing the caller's
#                              partition (tcp_conn_attribute_partition). The
#                              guard must go red at N10: an outbound
#                              connection with no partition charges nobody,
#                              so a flood looks like absence — the exact
#                              failure this increment exists to close.
#   P2_TOOTH=restore-sockets  — the descriptor is made to carry a live
#                              connection id (§6's tooth for increment 3).
#                              The guard must go red at N14: the descriptor's
#                              line is that it carries the listener's
#                              CONFIGURATION and never a connection id — a
#                              restore re-establishes the listener with every
#                              socket closed. A descriptor naming a conn id
#                              is exactly the half-open-socket-restored
#                              failure §6 says is not a thing to restore.
#   P2_TOOTH=no-quota-surface  — the connquota POST route stops dispatching;
#                              the guard reddens at N15, the surface the
#                              increment-4 live arm drives.
#   P2_TOOTH=no-quota          — §6's NAMED tooth for increment 4: the quota
#                              check leaves the admit path (the gate becomes
#                              constant-false). The guard reddens at N16
#                              while the reachability clauses N12-N14 stay
#                              green — the vacuity control, and the reason
#                              the phase exists.
#   P2_TOOTH=no-ci-live        — ci.yml's --live step deleted; N17 reddens
#                              alone (the gate cannot be deleted quietly).
#   P2_TOOTH=no-ci-order       — the step reordered ahead of its ISO build;
#                              N17b reddens alone while N17 holds green (the
#                              order half bites by itself).
#   plus one tooth per clause — every other clause N1-N17b reddens when the
#                              single property it carries is removed.
#
# ─── What a teeth smoke is for ─────────────────────────────────────────────
# A guard that has never been seen to fail is a guard nobody can trust. Each
# tooth below breaks ONE property in a throwaway copy of the tree and requires
# the guard to (a) exit 1 and (b) name THAT clause. Requiring the right named
# clause is the half that matters: a guard that reddened on everything, or on
# the wrong clause, would pass a tooth that only checked the exit code.
#
# The hermetic seam is the guard's own optional root argument: it inspects a
# repository root, defaulting to its own parent. This smoke builds a minimal
# root containing exactly the files the guard reads, applies one mutation,
# and runs the real guard against it — no network, no build, no boot.
#
# The last arm is the vacuity control: the guard must be GREEN on the real,
# unmutated tree. Without it, a guard that failed unconditionally would pass
# every tooth in this file.
#
# Usage:  bash tests/env_net_isolation_check_smoke.sh [P2_TOOTH=<name>]
#         (default: every tooth, in the order above)
#
# Exit: 0 every tooth bit and the vacuity control held, 1 a tooth did not,
#       2 misuse/precondition missing.
set -u
cd "$(dirname "$0")/.."
ROOT="$(pwd)"
GUARD="$ROOT/tests/env_net_isolation_check.sh"

[ -f "$GUARD" ] || { echo "ABORT: $GUARD not found — run from the repo root." >&2; exit 2; }

TOOTH_SET="${1:-${P2_TOOTH:-}}"
case "$TOOTH_SET" in
    ""|system-peer|network-rights|n-caps|pin-peer|system-profile|mint-arm|\
    no-register|no-skip|no-tick|no-makefile|no-refusal|no-hosttest|\
    no-attribution|no-admit-counter|no-listener-verbs|no-accept-attribution|\
    no-bind-port|descriptor-conn-id|no-quota-surface|no-quota|\
    no-ci-live|no-ci-order) ;;
    *) echo "ABORT: unknown P2_TOOTH='$TOOTH_SET' — see this file's header for the list" >&2; exit 2 ;;
esac
want_tooth() { [ -z "$TOOTH_SET" ] || [ "$TOOTH_SET" = "$1" ]; }

W="$(mktemp -d)"
cleanup() { rm -rf "$W"; }
trap cleanup EXIT
trap 'cleanup; exit 1' TERM INT

# The files the guard reads. Kept in one list so a tooth can never pass
# because the copy it mutated was missing something the guard needed.
SEED_FILES=(
    user/init/src/posix_manifest.rs
    user/init/src/entry.rs
    user/init/src/env_manager.rs
    kernel/cap.c
    kernel/net_socket_service.c
    kernel/net_socket_service.h
    kernel/console_service.c
    kernel/microkernel.c
    net/tcp_quota.c
    net/tcp_quota.h
    net/http.c
    kernel/syscall_dispatch.c
    user/shell.c
    Makefile
    tests/net_socket_service_host_test.c
    user/proto/src/env_proto.rs
    .github/workflows/ci.yml
)

seed() {   # seed <dir>
    rm -rf "$1"
    local f d
    # Derive each directory from SEED_FILES itself, so adding a file (with a
    # new subdirectory) never needs a second edit here to seed it correctly.
    for f in "${SEED_FILES[@]}"; do
        d="$(dirname "$f")"
        mkdir -p "$1/$d" || return 1
    done
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    return 0
}

replace() {   # replace <file> <old> <new> — python, so long lines need no escaping
    # ALL occurrences, not the first: a rule stated in a header and a doc
    # comment is still the rule, and replacing one copy would leave the tooth
    # biting nothing while looking aimed correctly. A pattern that is not
    # found is fatal — a tooth whose mutation did not apply would be asserted
    # against the unmutated tree and pass by accident, which is the one
    # failure mode this whole file exists to prevent, one level up.
    python3 - "$1" "$2" "$3" <<'PY' || { echo "ABORT: a tooth's mutation did not apply — aborting rather than asserting against an unmutated tree" >&2; exit 2; }
import sys
p, old, new = sys.argv[1], sys.argv[2], sys.argv[3]
s = open(p).read()
if old not in s:
    print(f"tooth pattern not found in {p}: {old!r}", file=sys.stderr)
    sys.exit(2)
open(p, "w").write(s.replace(old, new))
PY
}

passed=0
failed=0
tooth() {   # tooth <P2_TOOTH-name> <expected-clause> <label> <dir>
    local group="$1" clause="$2" label="$3" dir="$4" out rc
    out="$(bash "$GUARD" "$dir" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: ${clause}\."; then
        echo "ok:   P2_TOOTH=$group: $label (guard exits 1 naming '$clause')"
        passed=$((passed + 1))
    else
        echo "FAIL: P2_TOOTH=$group: $label did NOT bite — exit $rc, expected 1 with 'FAIL: $clause.'"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
}

echo "env_net_isolation_check_smoke — teeth for the P2 net-isolation guard (increments 2-4)"
echo "=================================================================="
echo

# ── A. A missing file is a refusal to evaluate, not a pass or a failure ────
seed "$W/a"; rm -f "$W/a/kernel/cap.c"
out="$(bash "$GUARD" "$W/a" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT:"; then
    echo "ok:   a missing cap.c aborts (exit 2) instead of reporting the wire sound"
    passed=$((passed + 1))
else
    echo "FAIL: a missing file did not abort (exit $rc)"
    echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# …and the increment-4 wiring surface obeys the same rule: a missing ci.yml
# is a refusal to evaluate (N17 could not be judged), never a pass — the
# property that keeps the new precondition honest.
seed "$W/a2"; rm -f "$W/a2/.github/workflows/ci.yml"
out="$(bash "$GUARD" "$W/a2" 2>&1)"; rc=$?
if [ "$rc" -eq 2 ] && printf '%s\n' "$out" | grep -q "^ABORT:"; then
    echo "ok:   a missing .github/workflows/ci.yml aborts (exit 2) instead of evaluating N17 against nothing"
    passed=$((passed + 1))
else
    echo "FAIL: a missing ci.yml did not abort (exit $rc)"
    echo "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

# ── P2_TOOTH=system-peer — the tooth §6 names for this increment ───────────
if want_tooth system-peer; then
    seed "$W/sp"
    sed -i 's/peer: Some("kernel.net.socket")/peer: Some("drv.network.0")/' \
        "$W/sp/user/init/src/posix_manifest.rs"
    tooth system-peer N1 "the tenant's network cap points at drv.network.0" "$W/sp"
fi

# ── the rest of N1: rights, the count, the positive pin ────────────────────
if want_tooth network-rights; then
    seed "$W/r"
    sed -i 's@rights: 0x7, // R | W | send — NET_@rights: 0x3, // R | W | send — NET_@' \
        "$W/r/user/init/src/posix_manifest.rs"
    tooth network-rights N1 "the network cap's rights shrink below its siblings" "$W/r"
fi
if want_tooth n-caps; then
    seed "$W/n"
    sed -i 's/n_caps: 4,/n_caps: 3,/' "$W/n/user/init/src/posix_manifest.rs"
    tooth n-caps N1 "n_caps stops counting the network cap" "$W/n"
fi
if want_tooth pin-peer; then
    seed "$W/pp"
    sed -i 's/assert_eq!(peer, "kernel.net.socket",/assert_eq!(peer, "drv.network.0",/' \
        "$W/pp/user/init/src/posix_manifest.rs"
    tooth pin-peer N1 "the rotated pin asserts the system sidecar instead" "$W/pp"
fi

# ── N2. the system profile is not to be 'fixed' along with the tenant's ────
if want_tooth system-profile; then
    seed "$W/sys"
    sed -i 's@peer: Some("drv.network.0")@peer: Some("kernel.net.socket")@' \
        "$W/sys/user/init/src/posix_manifest.rs"
    tooth system-profile N2 "the system profile's network cap is repointed too" "$W/sys"
fi

# ── N3. the mint arm ───────────────────────────────────────────────────────
if want_tooth mint-arm; then
    seed "$W/ma"
    sed -i '/sidecar_prefix(sc->peer_name, "kernel.net.socket")/d' "$W/ma/kernel/cap.c"
    tooth mint-arm N3 "the kernel.net.socket case in cap.c's kernel.* arm is deleted" "$W/ma"
fi

# ── N4. the registration ───────────────────────────────────────────────────
if want_tooth no-register; then
    seed "$W/nr"
    sed -i 's/net_socket_service_register(k_rd, k_wr,/net_socket_service_register_DISABLED(k_rd, k_wr,/' \
        "$W/nr/kernel/cap.c"
    tooth no-register N4 "the mint arm stops registering the channel with the service" "$W/nr"
fi

# ── N5. console_service's skip ─────────────────────────────────────────────
if want_tooth no-skip; then
    seed "$W/ns"
    sed -i 's/net_socket_service_kernel_slot((uint16_t)s)/net_socket_service_kernel_slot_DISABLED((uint16_t)s)/' \
        "$W/ns/kernel/console_service.c"
    tooth no-skip N5 "console_service stops skipping the socket channel's slot" "$W/ns"
fi

# ── N6. the tick ───────────────────────────────────────────────────────────
if want_tooth no-tick; then
    seed "$W/nt"
    sed -i '/net_socket_service_tick();/d' "$W/nt/kernel/microkernel.c"
    tooth no-tick N6 "the service is never ticked from the service poll" "$W/nt"
fi

# ── N7. the build ──────────────────────────────────────────────────────────
if want_tooth no-makefile; then
    seed "$W/nm"
    sed -i '\#kernel/net_socket_service.c#d' "$W/nm/Makefile"
    tooth no-makefile N7 "the service falls out of the kernel build" "$W/nm"
fi

# ── N8. the refusal rendering ──────────────────────────────────────────────
if want_tooth no-refusal; then
    seed "$W/nref"
    sed -i 's/refused by kernel.net.socket/denied by kernel.net.socket/' \
        "$W/nref/kernel/net_socket_service.c"
    tooth no-refusal N8 "the refusal stops naming the service it was refused by" "$W/nref"
fi

# ── N9. the host test's pin ────────────────────────────────────────────────
if want_tooth no-hosttest; then
    seed "$W/nht"
    sed -i 's/not admitted in P2 increment 3/not admitted in P2 increment X/' \
        "$W/nht/tests/net_socket_service_host_test.c"
    tooth no-hosttest N9 "the host test stops pinning the exact rendering" "$W/nht"
fi

# ── N10. the ADMIT path: attribution at the connect ─────────────────────────
# The load-bearing call: remove tcp_conn_attribute_partition and the connect
# would still open a connection — one that charges no partition, so a flood
# counts against nobody. That is the whole reason this increment exists.
if want_tooth no-attribution; then
    seed "$W/na"
    sed -i 's/tcp_conn_attribute_partition(/tcp_conn_attribute_partition_DISABLED(/' \
        "$W/na/kernel/net_socket_service.c"
    tooth no-attribution N10 "the connect stops attributing the caller's partition" "$W/na"
fi

# ── N11. the admission counters ───────────────────────────────────────────
# Without the admit counter an admitted connect is uncountable, so the
# unattributed tooth ("a flood counts against nobody") has nothing to bite.
if want_tooth no-admit-counter; then
    seed "$W/nac"
    sed -i 's/nss_admits_total/nss_admits_removed/' \
        "$W/nac/kernel/net_socket_service.c"
    tooth no-admit-counter N11 "the admission counter is gone (an admit is uncountable)" "$W/nac"
fi

# ── N12. the listener verbs reach the kernel stack ──────────────────────────
if want_tooth no-listener-verbs; then
    seed "$W/nlv"
    sed -i 's/int lid = tcp_listen(sk->lport);/int lid = tcp_listen_DISABLED(sk->lport);/' \
        "$W/nlv/kernel/net_socket_service.c"
    tooth no-listener-verbs N12 "the listen no longer reaches the kernel stack (tcp_listen)" "$W/nlv"
fi

# ── N13. the accept charges the accepted conn to the caller's partition ─────
if want_tooth no-accept-attribution; then
    seed "$W/naa"
    sed -i 's/tcp_conn_attribute_partition(found, e->partition)/tcp_conn_attribute_partition(found, SOME_OTHER_PARTITION)/' \
        "$W/naa/kernel/net_socket_service.c"
    tooth no-accept-attribution N13 "the accept stops charging the accepted conn to the caller's own partition" "$W/naa"
fi

# ── N12. the bind claims the port on the channel's own socket ────────────────
if want_tooth no-bind-port; then
    seed "$W/nbp"
    sed -i 's/        sk->lport = port;/        \/* bind no longer claims the port *\//' \
        "$W/nbp/kernel/net_socket_service.c"
    tooth no-bind-port N12 "the bind no longer claims the port on the channel's socket" "$W/nbp"
fi

# ── P2_TOOTH=restore-sockets — §6's tooth for increment 3 ──────────────────
# The descriptor is made to carry a live connection id. N14 must redden: the
# descriptor's line is listener CONFIG, never a conn id — a restore
# re-establishes the listener with every socket closed.
if want_tooth descriptor-conn-id; then
    seed "$W/dci"
    sed -i 's/    pub n_chans: u32,/    pub conn_id: u32,\n    pub n_chans: u32,/' \
        "$W/dci/user/proto/src/env_proto.rs"
    tooth descriptor-conn-id N14 "the descriptor now carries a live connection id" "$W/dci"
fi

# ── P2 increment 4's teeth: the quota surface, the gate, the CI wiring ────

# N15. the POST route no longer dispatches: the live arm's way to set a
# quota is gone, and N15 must name it — the surface "already exists" only as
# long as the route is wired.
if want_tooth no-quota-surface; then
    seed "$W/nqs"
    replace "$W/nqs/net/http.c" \
        'if (!strcmp(path, "/api/partition/connquota")) {' \
        'if (0) {   /* the route is gone */'
    tooth no-quota-surface N15 "the connquota POST route no longer dispatches" "$W/nqs"
fi

# ── P2_TOOTH=no-quota — §6's named tooth for increment 4 ──────────────────
# The quota check leaves the admit path: the gate's comparison becomes a
# constant-false branch. The module still compiles and attribution still
# runs, but nothing is ever over quota — A's flood would sail past and B
# would starve with no refusal to name. Narrowness IS the property: N16
# reddens while the REACHABILITY clauses N12-N14 stay green, the vacuity
# control the roadmap names and the reason the phase exists.
if want_tooth no-quota; then
    seed "$W/nq"
    replace "$W/nq/net/tcp_quota.c" \
        'if (quota != 0 && partition_conn_count[partition_id] >= quota) {' \
        'if (0) {   /* the quota check was removed from the admit path */'
    out="$(bash "$GUARD" "$W/nq" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: N16\." && \
       ! printf '%s\n' "$out" | grep -qE "^FAIL: N1[234]\."; then
        echo "ok:   P2_TOOTH=no-quota: the quota check leaves the admit path — N16 reddens while the reachability clauses N12-N14 stay green"
        passed=$((passed + 1))
    else
        echo "FAIL: P2_TOOTH=no-quota did NOT bite as attributed — exit $rc (want 1, 'FAIL: N16.', no 'FAIL: N12/N13/N14')"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
fi

# N17/N17b. the flood evidence's wiring — P1b's S13 pair, point for point:
# the step deleted reddens N17 alone; the step reordered ahead of its ISO
# reddens N17b alone (a boot guard ahead of its image aborts before booting,
# reddening for the wrong reason while proving nothing about the flood).
if want_tooth no-ci-live; then
    seed "$W/ncl"
    replace "$W/ncl/.github/workflows/ci.yml" \
        'run: bash tests/env_net_isolation_check.sh --live' \
        'run: bash tests/env_net_isolation_check.sh'
    out="$(bash "$GUARD" "$W/ncl" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: N17\." && \
       ! printf '%s\n' "$out" | grep -q "^FAIL: N17b\."; then
        echo "ok:   P2_TOOTH=no-ci-live: ci.yml without the --live arm reddens N17 (and only it) — the gate cannot be deleted quietly"
        passed=$((passed + 1))
    else
        echo "FAIL: P2_TOOTH=no-ci-live did NOT bite narrowly — exit $rc (want 1 with 'FAIL: N17.' and no 'FAIL: N17b.')"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
fi
if want_tooth no-ci-order; then
    seed "$W/nco"
    replace "$W/nco/.github/workflows/ci.yml" \
        '- name: Build the kernel ISO (host gcc/ld as the toolchain)' \
        '- name: P2 live arm before its ISO (the misorder N17b catches)
        run: bash tests/env_net_isolation_check.sh --live
      - name: Build the kernel ISO (host gcc/ld as the toolchain)'
    out="$(bash "$GUARD" "$W/nco" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: N17b\." && \
       printf '%s\n' "$out" | grep -q "^ok:   N17\."; then
        echo "ok:   P2_TOOTH=no-ci-order: a live arm ordered before the ISO build reddens N17b while N17 holds green — the order half bites alone"
        passed=$((passed + 1))
    else
        echo "FAIL: P2_TOOTH=no-ci-order did NOT bite as attributed — exit $rc (want 1, 'FAIL: N17b.', 'ok:   N17.')"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
fi

# ── the vacuity control: the REAL tree must be GREEN ───────────────────────
out="$(bash "$GUARD" 2>&1)"; rc=$?
if [ "$rc" -eq 0 ]; then
    echo "ok:   the untouched tree passes (the guard is not failing unconditionally)"
    passed=$((passed + 1))
else
    echo "FAIL: the untouched tree does NOT pass — the guard is not measuring what it claims (exit $rc)"
    printf '%s\n' "$out" | sed 's/^/      /'
    failed=$((failed + 1))
fi

echo
echo "=== $passed passed, $failed failed ==="
[ "$failed" -eq 0 ]
