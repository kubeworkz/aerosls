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
#   plus one tooth per clause — every other clause N1-N9 reddens when the
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
    no-register|no-skip|no-tick|no-makefile|no-refusal|no-hosttest) ;;
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
    Makefile
    tests/net_socket_service_host_test.c
)

seed() {   # seed <dir>
    rm -rf "$1"
    mkdir -p "$1/user/init/src" "$1/kernel" "$1/tests"
    local f
    for f in "${SEED_FILES[@]}"; do
        cp "$ROOT/$f" "$1/$f" || return 1
    done
    return 0
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

echo "env_net_isolation_check_smoke — teeth for the P2 increment-1 guard"
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
    sed -i 's/nothing admits yet (P2 increment 1)/nothing admits yet/' \
        "$W/nht/tests/net_socket_service_host_test.c"
    tooth no-hosttest N9 "the host test stops pinning the exact rendering" "$W/nht"
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
