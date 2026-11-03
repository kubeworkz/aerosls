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
#   P2_TOOTH=no-birth-default  — §11 Q4's default leaves the mint: the
#                              partition is created unbounded again, and N18
#                              names the mint site while N19 holds green (the
#                              default and the authority are different teeth).
#   P2_TOOTH=no-restore-default — the restore path stops re-applying it: the
#                              two partition lifetimes disagree (a rebooted
#                              tenant unbounded, a fresh one capped), N18.
#   P2_TOOTH=no-birth-line     — the kernel's birth line stops naming the
#                              default; N18 reddens on the evidence N19b reads.
#   P2_TOOTH=ungated-quota-set — the 277 arm stops checking the caller's
#                              partition: a tenant could undo the default, N19.
#   P2_TOOTH=no-quota-role     — the HTTP route stops requiring DB_ADMIN: the
#                              kernel-context path is ungated, N19.
#   P2_TOOTH=ungated-cpu-weight / ungated-storage-quota — the sibling setters
#                              (273, 275) stop asking the same authority
#                              helper; the class of finding N19 now covers is
#                              reopened one setter at a time, N19.
#   P2_TOOTH=no-cpuweight-role / no-storage-role — the matching HTTP routes
#                              stop requiring DB_ADMIN, N19.
#   P2_TOOTH=ungated-frame-quota / ungated-assign / ungated-destroy /
#            ungated-pause / ungated-resume — the partition-wide arms (213,
#                              211, 214, 215, 216) stop asking the same
#                              authority helper; N20 names the arm that lost
#                              it. One per arm, because a class that can be
#                              reopened one operation at a time is not gated.
#   P2_TOOTH=no-frame-quota-role / no-assign-role / no-destroy-role /
#            no-pause-role — the matching HTTP routes stop requiring DB_ADMIN,
#                              N20. Pause and resume share one handler, so its
#                              gate has one tooth covering both routes — there
#                              is no second copy of the gate to remove, and a
#                              tooth that claimed to remove one would be
#                              asserting against nothing.
#   P2_TOOTH=ungated-create / ungated-migrate — the last two partition-wide
#                              arms (210, 218) lose their caller check; N21.
#                              Create's tooth is the one that proves a tenant
#                              cannot mint a partition, migrate's that it
#                              cannot move one.
#   P2_TOOTH=no-create-route-role — /api/partitions stops requiring DB_ADMIN.
#                              That gate existed from the route's first
#                              version but no clause asserted it until N21,
#                              so this is the tooth that makes the claim
#                              falsifiable rather than merely true.
#   plus one tooth per clause — every other clause N1-N21 reddens when the
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
    no-ci-live|no-ci-order|no-birth-default|no-restore-default|\
    no-birth-line|ungated-quota-set|no-quota-role|ungated-cpu-weight|\
    ungated-storage-quota|no-cpuweight-role|no-storage-role|\
    ungated-frame-quota|ungated-assign|ungated-destroy|ungated-pause|\
    ungated-resume|no-frame-quota-role|no-assign-role|no-destroy-role|\
    no-pause-role|ungated-create|ungated-migrate|no-create-route-role) ;;
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
    kernel/partition.c
    kernel/partition.h
    kernel/persist.c
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
    sed -i 's/n_caps: 5,/n_caps: 4,/' "$W/n/user/init/src/posix_manifest.rs"
    tooth n-caps N1 "n_caps stops counting one of the five tenant caps" "$W/n"
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

# ── P2 increment 5 / §11 Q4's default and authority ───────────────────────

# N18. the default leaves the MINT — a partition created after boot is
# unbounded again while a restored one keeps its ceiling. N19 must hold
# green: the authority rule is a different property with its own tooth.
if want_tooth no-birth-default; then
    seed "$W/nbd"
    replace "$W/nbd/kernel/partition.c" \
        'pq_born_bounded((uint32_t)i);' \
        '(void)0;   /* the mint no longer bounds the partition */'
    out="$(bash "$GUARD" "$W/nbd" 2>&1)"; rc=$?
    if [ "$rc" -eq 1 ] && printf '%s\n' "$out" | grep -q "^FAIL: N18\." && \
       ! printf '%s\n' "$out" | grep -q "^FAIL: N19\."; then
        echo "ok:   P2_TOOTH=no-birth-default: the mint drops the tenant default — N18 reddens while N19 (who may change it) stays green"
        passed=$((passed + 1))
    else
        echo "FAIL: P2_TOOTH=no-birth-default did NOT bite as attributed — exit $rc (want 1, 'FAIL: N18.', no 'FAIL: N19.')"
        printf '%s\n' "$out" | sed 's/^/      /'
        failed=$((failed + 1))
    fi
fi

# N18. the default leaves the RESTORE path — the other way a partition comes
# into existence (persist_restore_all() reads the table directly). A rebooted
# tenant unbounded while a fresh one is capped is the split this tooth makes.
if want_tooth no-restore-default; then
    seed "$W/nrd"
    replace "$W/nrd/kernel/persist.c" \
        'partition_apply_conn_quota_defaults();' \
        '(void)0;   /* the restore path no longer re-applies the default */'
    tooth no-restore-default N18 "the restore path stops re-applying the tenant default" "$W/nrd"
fi

# N18. the birth line stops naming the default — the property still holds,
# but the evidence N19b reads (and an operator's boot log) goes silent.
if want_tooth no-birth-line; then
    seed "$W/nbl"
    replace "$W/nbl/net/tcp_quota.c" \
        '(tenant default; 0 = explicitly unlimited)' \
        '(default)'
    tooth no-birth-line N18 "the birth line stops naming the default" "$W/nbl"
fi

# N19. the 277 arm stops checking the caller's partition — the arm shipped
# with no check at all, so this is the regression to watch for: the default
# is set and then immediately undoable by the tenant it bounds.
if want_tooth ungated-quota-set; then
    seed "$W/uqs"
    replace "$W/uqs/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("TCP-QUOTA", "change a connection quota"))' \
        'if (0)   /* the conn-quota arm lost its caller check */'
    tooth ungated-quota-set N19 "the conn-quota arm stops checking the caller's partition" "$W/uqs"
fi

# N19. the HTTP route stops requiring DB_ADMIN — kernel context has no
# process, so this is the one path the dispatch check cannot cover.
if want_tooth no-quota-role; then
    seed "$W/nqr"
    # Anchored to this handler's own signature, the same discipline the
    # sibling route teeth use: since the lifecycle routes took the same gate,
    # a bare 'req_role > ROLE_DB_ADMIN' replacement would remove every route's
    # gate at once and prove nothing about this one. The mutation is the role
    # parameter itself (this handler carries a comment between its JSONBuf and
    # its gate, so the gate line is not reachable by a span anchored on the
    # signature) — the gate cannot be judged if the role never arrives.
    replace "$W/nqr/net/http.c" \
$'static int api_partition_connquota_post(const char* body, char* buf, int max,\n                                        SLSRole req_role) {' \
$'static int api_partition_connquota_post(const char* body, char* buf, int max) {   /* the role never arrives */'
    tooth no-quota-role N19 "the connquota handler stops taking the resolved role, so it cannot be gated" "$W/nqr"
fi

# N19. the sibling setters (273 CPU weight, 275 storage quota) stop asking
# the same authority helper. One tooth each, so the class cannot be reopened
# one setter at a time without a named red.
if want_tooth ungated-cpu-weight; then
    seed "$W/ucw"
    replace "$W/ucw/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("SCHED", "change a CPU weight"))' \
        'if (0)   /* the CPU-weight arm lost its caller check */'
    tooth ungated-cpu-weight N19 "the CPU-weight setter stops checking the caller's partition" "$W/ucw"
fi
if want_tooth ungated-storage-quota; then
    seed "$W/usq"
    replace "$W/usq/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("STORAGE-QUOTA", "change a storage quota"))' \
        'if (0)   /* the storage-quota arm lost its caller check */'
    tooth ungated-storage-quota N19 "the storage-quota setter stops checking the caller's partition" "$W/usq"
fi

# N19. the matching control-plane routes stop requiring DB_ADMIN. Each
# mutation is anchored to its own handler's signature, so the tooth cannot
# redden by removing a different route's gate.
if want_tooth no-cpuweight-role; then
    seed "$W/ncwr"
    replace "$W/ncwr/net/http.c" \
$'static int api_partition_cpuweight_post(const char* body, char* buf, int max,\n                                        SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_cpuweight_post(const char* body, char* buf, int max,\n                                        SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-cpuweight-role N19 "the CPU-weight POST stops requiring DB_ADMIN" "$W/ncwr"
fi
if want_tooth no-storage-role; then
    seed "$W/nsr"
    replace "$W/nsr/net/http.c" \
$'static int api_partition_storagequota_post(const char* body, char* buf, int max,\n                                           SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_storagequota_post(const char* body, char* buf, int max,\n                                           SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-storage-role N19 "the storage-quota POST stops requiring DB_ADMIN" "$W/nsr"
fi

# N20. the partition-wide arms (213 frame quota, 211 assign, 214 destroy,
# 215 pause, 216 resume) stop asking the authority helper. One tooth each: a
# class that can be reopened one operation at a time without a named red is
# not closed, and 211's tooth is the one that proves the gate's own door is
# shut (an ungated assign puts a tenant's uid in partition 0).
if want_tooth ungated-frame-quota; then
    seed "$W/ufq"
    replace "$W/ufq/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("FRAME-QUOTA", "change a frame quota"))' \
        'if (0)   /* the frame-quota arm lost its caller check */'
    tooth ungated-frame-quota N20 "the frame-quota arm stops checking the caller's partition" "$W/ufq"
fi
if want_tooth ungated-assign; then
    seed "$W/uas"
    replace "$W/uas/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("PARTITION", "assign a uid to a partition"))' \
        'if (0)   /* the assign arm lost its caller check */'
    tooth ungated-assign N20 "the assign arm stops checking the caller's partition (the gate's door)" "$W/uas"
fi
if want_tooth ungated-destroy; then
    seed "$W/uds"
    replace "$W/uds/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("PARTITION", "destroy a partition"))' \
        'if (0)   /* the destroy arm lost its caller check */'
    tooth ungated-destroy N20 "the destroy arm stops checking the caller's partition" "$W/uds"
fi
if want_tooth ungated-pause; then
    seed "$W/upa"
    replace "$W/upa/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("PARTITION", "pause a partition"))' \
        'if (0)   /* the pause arm lost its caller check */'
    tooth ungated-pause N20 "the pause arm stops checking the caller's partition" "$W/upa"
fi
if want_tooth ungated-resume; then
    seed "$W/ure"
    replace "$W/ure/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("PARTITION", "resume a partition"))' \
        'if (0)   /* the resume arm lost its caller check */'
    tooth ungated-resume N20 "the resume arm stops checking the caller's partition" "$W/ure"
fi

# N20. the matching routes stop requiring DB_ADMIN, each mutation anchored to
# its own handler's signature so it cannot redden by removing a neighbour's
# gate. Pause and resume share api_partition_pause_post(), so one tooth covers
# both routes: there is no second gate to delete.
if want_tooth no-frame-quota-role; then
    seed "$W/nfqr"
    replace "$W/nfqr/net/http.c" \
$'static int api_partition_quota_post(const char* body, char* buf, int max,\n                                    SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_quota_post(const char* body, char* buf, int max,\n                                    SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-frame-quota-role N20 "the frame-quota POST stops requiring DB_ADMIN" "$W/nfqr"
fi
if want_tooth no-assign-role; then
    seed "$W/nasr"
    replace "$W/nasr/net/http.c" \
$'static int api_partition_assign_post(const char* body, char* buf, int max,\n                                     SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_assign_post(const char* body, char* buf, int max,\n                                     SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-assign-role N20 "the assign POST stops requiring DB_ADMIN" "$W/nasr"
fi
if want_tooth no-destroy-role; then
    seed "$W/ndr"
    replace "$W/ndr/net/http.c" \
$'static int api_partition_destroy_post(const char* body, char* buf, int max,\n                                      SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_destroy_post(const char* body, char* buf, int max,\n                                      SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-destroy-role N20 "the destroy POST stops requiring DB_ADMIN" "$W/ndr"
fi
if want_tooth no-pause-role; then
    seed "$W/npr"
    replace "$W/npr/net/http.c" \
$'static int api_partition_pause_post(const char* body, char* buf, int max, int resume,\n                                    SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_pause_post(const char* body, char* buf, int max, int resume,\n                                    SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-pause-role N20 "the pause/resume POST stops requiring DB_ADMIN (both routes share this handler)" "$W/npr"
fi

# N21. the last two partition-wide arms. Create is the one a tenant had the
# most direct use for (a frame-pool share and a table slot), and migrate is
# the one whose own guard covers its target rather than its caller — remove
# either check and the arm answers a ring-3 caller again.
if want_tooth ungated-create; then
    seed "$W/ucr"
    replace "$W/ucr/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("PARTITION", "create a partition"))' \
        'if (0)   /* the create arm lost its caller check */'
    tooth ungated-create N21 "the create arm stops checking the caller's partition" "$W/ucr"
fi
if want_tooth ungated-migrate; then
    seed "$W/umg"
    replace "$W/umg/kernel/syscall_dispatch.c" \
        'if (!partition_control_may_act("PARTITION", "migrate a partition"))' \
        'if (0)   /* the migrate arm lost its caller check */'
    tooth ungated-migrate N21 "the migrate arm stops checking the caller's partition" "$W/umg"
fi

# N21. /api/partitions loses the gate it has always had — the clause that
# asserted it is new here, so this tooth is what keeps that assertion honest.
if want_tooth no-create-route-role; then
    seed "$W/ncr"
    replace "$W/ncr/net/http.c" \
$'static int api_partition_create_post(const char* body, char* buf, int max, SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (req_role > ROLE_DB_ADMIN) {' \
$'static int api_partition_create_post(const char* body, char* buf, int max, SLSRole req_role) {\n    JSONBuf j = { buf, 0, max };\n    if (0 > 1) {   /* the role gate was removed */'
    tooth no-create-route-role N21 "/api/partitions stops requiring DB_ADMIN" "$W/ncr"
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
