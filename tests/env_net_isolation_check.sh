#!/usr/bin/env bash
# tests/env_net_isolation_check.sh — POSIX-Environments v0.2 §6's guard, P2's
# first increment: the tenant cap, the kernel-owned name, the refusal wire.
#
# ─── Why this exists ──────────────────────────────────────────────────────
# P2 lifts the tenant profile's ceiling (§6): the manifest gains a `network`
# Chan cap whose peer is the KERNEL-OWNED name "kernel.net.socket", never
# `drv.network.0` — that sidecar sits in the system partition, and a channel
# to it would cross the LPAR Phase 11 IPC boundary (E2's scoped registry
# refuses it at creation — the system-peer tooth). cap_create_sidecar()'s
# kernel.* arm grows the case beside kernel.env.control and kernel.env.console,
# and the service behind the name speaks NET_* and answers every verb with a
# refusal BY NAME. Nothing admits yet: the channel exists, is typed, and fails
# honestly — the wire increments 2-4 write admission into.
#
# ─── Modes ────────────────────────────────────────────────────────────────
#   (no arguments)      the SOURCE clauses over this script's own repository root
#   --live              the source clauses, then the boot arm: a tenant console's
#                       `nc` reaches the service and is refused by name
#                       (needs the built ISO + QEMU; opt-in, never the default)
#   positional ROOT     the source clauses over another tree (the smoke's own
#                       hermetic seam; every real caller passes nothing)
#
# ─── The source clauses ───────────────────────────────────────────────────
#   N1. the tenant manifest: `network`, rights 0x7, peer EXACTLY
#       kernel.net.socket (never drv.network.0 — the system-peer tooth pinned
#       at the source), n_caps 4, the rotated pin test, and BOTH callers (E3
#       boot spawn, control-plane ENV_CREATE) grant the tenant manifest;
#   N2. the SYSTEM profile is untouched: its own network -> drv.network.0
#       stays where it was (only the tenant profile was to change);
#   N3. the mint arm: cap.c's kernel.net.socket case is there, and so is the
#       E2 scoped-registry branch a drv.network.0 peer falls to — the
#       boundary, not the API;
#   N4. the registration carries the caller's identity — partition, pid and
#       name — which is what the refusal later names;
#   N5. console_service skips the socket channel's kernel slot (NET_* frames
#       are not console text and must not reach the serial transcript);
#   N6. the tick runs in the non-IRQ service poll, with env_console_tick;
#   N7. the Makefile compiles the service;
#   N8. the wire refuses by name: NET_FLAG_ERROR + NET_ERR_CAP (9) with the
#       rendered text, and NET_INFO answers max_sockets 0 (nothing admits);
#   N9. the host test links the REAL service and pins the exact rendering
#       (one rendering, three callers: host test, serial line, live clause).
#
# ─── The live arm (--live) ────────────────────────────────────────────────
#   L1. the created tenant environment's channel registers under the
#       kernel-owned name (kernel TX — "[NET-SOCKET] aerosls.posix.1
#       (partition P, pid Q) wired: kernel ends …" — pid agreeing with the
#       control plane's posix_pid: kernel's own account and the API's);
#   L2. its console's `nc <host> <port>` reaches the service and is refused
#       BY NAME: one serial line carries both the caller's identity and
#       "NET_SOCKET refused by kernel.net.socket — nothing admits yet …".
#       Needs no peer: socket() itself is refused before any connect;
#   L3. no environment ever reports NETBOOT FAILED — every handshake got the
#       honest NET_INFO answer (a service that never replies makes each
#       sidecar's bounded retry loop print that line).
#
# Teeth: tests/env_net_isolation_check_smoke.sh proves every clause above
# reddens when the property it carries is removed (P2_TOOTH=system-peer is
# the tooth §6 names for this increment), and that this guard is GREEN on
# the real tree — the vacuity control without which a guard that failed
# unconditionally would pass every tooth.
#
# GUARD-KIND: host (plain bash over the tree; --live adds QEMU + the ISO and
# is opt-in — the smoke proves every source tooth on every push with no build).
#
# Exit: 0 if every clause held, 1 if one failed, 2 a precondition missing.
set -u
cd "$(dirname "$0")/.."   # repo root: default ROOT, and where --live boots from

ROOT=""
LIVE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --live) LIVE=1 ;;
        -*)     echo "ABORT: unknown argument '$1' (usage: $0 [ROOT] [--live])" >&2; exit 2 ;;
        *)      if [ -n "$ROOT" ]; then
                    echo "ABORT: more than one ROOT ('$ROOT' and '$1')" >&2; exit 2
                fi
                ROOT="$1" ;;
    esac
    shift
done
if [ "$LIVE" -eq 1 ] && [ -n "$ROOT" ]; then
    echo "ABORT: --live inspects the real repository; the ROOT seam is source-only" >&2
    exit 2
fi
[ -n "$ROOT" ] || ROOT="$(pwd)"

ok()   { echo "ok:   $*"; }
bad()  { echo "FAIL: $*"; fail=1; }
note() { echo "note: $*"; }
fail=0

MF="$ROOT/user/init/src/posix_manifest.rs"
INITE="$ROOT/user/init/src/entry.rs"
ENVM="$ROOT/user/init/src/env_manager.rs"
CAP="$ROOT/kernel/cap.c"
SVC="$ROOT/kernel/net_socket_service.c"
SVCH="$ROOT/kernel/net_socket_service.h"
CONS="$ROOT/kernel/console_service.c"
MMK="$ROOT/kernel/microkernel.c"
MKF="$ROOT/Makefile"
TST="$ROOT/tests/net_socket_service_host_test.c"

# Precondition: every file the clauses read. Missing is a refusal to evaluate
# (exit 2), not a pass and not a finding — the same rule the restore guard's
# smoke pins with its arm A.
for f in "$MF" "$INITE" "$ENVM" "$CAP" "$SVC" "$SVCH" "$CONS" "$MMK" "$MKF" "$TST"; do
    [ -f "$f" ] || { echo "ABORT: ${f#"$ROOT"/} missing — the guard cannot be evaluated" >&2; exit 2; }
done

# has <file> <fixed string> — 0 when present. Fixed-string on purpose: every
# anchor below is source text, not a pattern.
has() { grep -qF -- "$2" "$1" 2>/dev/null; }
# hasstr <text> <fixed string> — the same, over an extracted body.
hasstr() { printf '%s\n' "$1" | grep -qF -- "$2" 2>/dev/null; }
# fn_body <file> <literal first line> — from the line that starts with the
# pattern to the first '^}' at column 0 (the shape every fn in these files has).
fn_body() {
    awk -v pat="$2" 'index($0, pat) == 1 { grab = 1 }
                     grab { print }
                     grab && /^}/ { exit }' "$1"
}
# clause <mark> <id> <label> — exactly one ok line when nothing under that
# clause failed; the FAIL lines above it are already prefixed with the id.
clause() { [ "$fail" -eq "$1" ] && ok "$2. $3"; }

TENANT="$(fn_body "$MF" 'pub fn build_posix_manifest_tenant(')"
SYSTEM="$(fn_body "$MF" 'pub fn build_posix_manifest(')"

# ── N1. the tenant manifest's network cap, and the rotated pin ────────────
c1=$fail
hasstr "$TENANT" 'peer: Some("kernel.net.socket"),' ||
    bad "N1. tenant manifest: the network cap's peer must be exactly kernel.net.socket"
printf '%s\n' "$TENANT" | grep -A2 -F 'name: "network"' | grep -qF 'rights: 0x7' ||
    bad "N1. tenant manifest: the network cap's rights must be 0x7 (R|W|send, beside console and ramdisk)"
hasstr "$TENANT" 'n_caps: 4,' ||
    bad "N1. tenant manifest: n_caps must count the fourth (network) cap"
if hasstr "$TENANT" 'peer: Some("drv.network.0"),'; then
    bad "N1. tenant manifest: the network cap must never peer at drv.network.0 — the system partition's sidecar (the system-peer tooth)"
fi
has "$MF" 'fn tenant_profile_is_budget_console_ramdisk_and_kernel_socket(' ||
    bad "N1. the pin test must be renamed to count caps and name the kernel socket"
has "$MF" 'assert_eq!(peer, "kernel.net.socket",' ||
    bad "N1. the pin test must assert the peer is exactly kernel.net.socket (the positive half of the rotated pin)"
ALINE="$(grep -F 'for absent in [' "$MF" | head -1)"
if [ -z "$ALINE" ]; then
    bad "N1. the pin test's absent-list is gone — the hardware-absence half of the pin cannot be evaluated"
else
    case "$ALINE" in
        *'"network"'*) bad "N1. the pin test must not list network among the ABSENT caps — it is present now (that list is the old pin)" ;;
    esac
fi
has "$INITE" 'build_posix_manifest_tenant(' ||
    bad "N1. the E3 boot spawn no longer grants the tenant manifest"
has "$ENVM" 'build_posix_manifest_tenant(' ||
    bad "N1. the control plane's ENV_CREATE no longer grants the tenant manifest"
clause "$c1" N1 "tenant manifest: network, rights 0x7, peer kernel.net.socket, n_caps 4, pinned, both callers grant it"

# ── N2. the SYSTEM profile is untouched ───────────────────────────────────
c2=$fail
hasstr "$SYSTEM" 'name: "network",' ||
    bad "N2. system profile: its own network cap is gone — only the TENANT profile was to change"
if ! printf '%s\n' "$SYSTEM" | grep -A4 -F 'name: "network"' | grep -qF 'peer: Some("drv.network.0"),'; then
    bad "N2. system profile: network must keep peering at drv.network.0 (the system sidecar) — deliberately untouched by P2"
fi
clause "$c2" N2 "system profile untouched: network -> drv.network.0 stays where it was"

# ── N3. the mint arm, and the boundary branch a system peer falls to ──────
c3=$fail
has "$CAP" 'sidecar_prefix(sc->peer_name, "kernel.net.socket")' ||
    bad "N3. cap.c: the kernel.net.socket mint arm (beside kernel.env.control/console) is gone"
has "$CAP" 'sidecar_registry_resolve(sc->peer_name,' ||
    bad "N3. cap.c: the E2 scoped-registry branch — what a drv.network.0 peer falls to, refused for crossing partitions — is gone"
clause "$c3" N3 "mint arm present; a system peer still falls to the scoped registry (the boundary)"

# ── N4. the registration carries the caller's identity ────────────────────
c4=$fail
has "$CAP" 'net_socket_service_register(k_rd, k_wr,' ||
    bad "N4. cap.c: the mint arm must register the channel with the service"
ARM="$(grep -A8 -F 'sidecar_prefix(sc->peer_name, "kernel.net.socket")' "$CAP" 2>/dev/null)"
hasstr "$ARM" 'pd->partition_id,' ||
    bad "N4. registration must carry the caller's partition"
hasstr "$ARM" 'pd->pid, pd->name' ||
    bad "N4. registration must carry the sidecar's pid and name — the refusal names the caller with them"
clause "$c4" N4 "registration carries partition, pid and name under the kernel-owned name"

# ── N5. console_service skips the socket channel's kernel slot ────────────
c5=$fail
has "$CONS" '#include "net_socket_service.h"' ||
    bad "N5. console_service.c: the service's header is not included"
has "$CONS" 'net_socket_service_kernel_slot((uint16_t)s)' ||
    bad "N5. console_service.c: the socket channel's kernel slot must be skipped — NET_* frames are not console text and must not reach the serial transcript"
clause "$c5" N5 "console_service skips the socket channel's kernel slot"

# ── N6. the tick runs in the non-IRQ service poll, with env_console_tick ──
c6=$fail
has "$MMK" '#include "net_socket_service.h"' ||
    bad "N6. microkernel.c: the service's header is not included"
NT="$(grep -nF 'net_socket_service_tick();' "$MMK" | head -1 | cut -d: -f1)"
ET="$(grep -nF 'env_console_tick();' "$MMK" | head -1 | cut -d: -f1)"
if [ -z "$NT" ]; then
    bad "N6. microkernel.c: the service is never ticked — registered channels would never be drained"
elif [ -n "$ET" ] && [ "$NT" -le "$ET" ]; then
    bad "N6. microkernel.c: the socket tick must run in the service poll with (after) env_console_tick, not somewhere else"
fi
clause "$c6" N6 "net_socket_service_tick() runs in the non-IRQ service poll beside env_console_tick()"

# ── N7. the Makefile compiles the service ─────────────────────────────────
c7=$fail
has "$MKF" 'kernel/net_socket_service.c' ||
    bad "N7. Makefile: kernel/net_socket_service.c is not compiled into the kernel"
clause "$c7" N7 "the service is compiled into the kernel"

# ── N8. the wire refuses by name, and NET_INFO answers honestly ───────────
c8=$fail
has "$SVC" 'refused by kernel.net.socket' ||
    bad "N8. net_socket_service.c: the refusal does not name the service"
has "$SVC" 'nss_wire_u16(reply + 12, NSS_ERR_FLAG);' ||
    bad "N8. net_socket_service.c: the refusal reply must carry NET_FLAG_ERROR"
has "$SVC" 'nss_wire_u16(reply + 16, NSS_STATUS_CAP);' ||
    bad "N8. net_socket_service.c: the refusal reply's status must be NET_ERR_CAP"
has "$SVC" '#define NSS_STATUS_CAP 9u' ||
    bad "N8. net_socket_service.c: NET_ERR_CAP must stay 9 (the status the client parses)"
has "$SVC" '#define NSS_MAX_SOCKS 0u' ||
    bad "N8. net_socket_service.c: NET_INFO must answer max_sockets 0 — nothing admits yet (P2 increment 1)"
has "$SVC" 'net_socket_verb_name(' ||
    bad "N8. net_socket_service.c: the verb-name table (the refusal's 'by name') is gone"
clause "$c8" N8 "the wire refuses by name (NET_FLAG_ERROR + NET_ERR_CAP 9 + rendered text); NET_INFO answers honestly"

# ── N9. the host test links the real service and pins the rendering ───────
c9=$fail
has "$TST" 'kernel/net_socket_service.c' ||
    bad "N9. the host test must link the REAL service (kernel/net_socket_service.c), not a reimplementation"
has "$TST" 'refused by kernel.net.socket — nothing admits yet (P2 increment 1)' ||
    bad "N9. the host test must pin the exact refusal rendering the tick logs and the live clause greps"
clause "$c9" N9 "the host test links the real service and pins the one refusal rendering"

# ── the live arm ──────────────────────────────────────────────────────────
if [ "$LIVE" -eq 1 ]; then
    if [ "$fail" -ne 0 ]; then
        note "source clauses already red — not booting (fix the source first)"
    else
        # ── boot: the same QEMU + grub drive the E6 attach guard uses ──────
        ISO="${P2_ISO:-sls_operating_system.iso}"
        LOG=/tmp/aerosls_p2_net_boot.log
        ENTRY="${P2_BOOT_ENTRY:-3}"
        WINDOW_S="${P2_WINDOW_S:-180}"
        WAIT_S="${P2_WAIT_S:-60}"
        ATTACH_WAIT_S="${P2_ATTACH_WAIT_S:-45}"
        SMP="${P2_SMP:-4}"
        # The API's own token/role model: DB_ADMIN, what partition create and
        # an environment's create and console write all require (the same
        # token the other HTTP-driven boot checks use).
        TOKEN=deadbeef01234567cafebabe76543210

        [ -f "$ISO" ] || { echo "ABORT: $ISO missing — build it with: make x86-iso (with sidecars.cpio present)" >&2; exit 2; }
        command -v qemu-system-x86_64 >/dev/null 2>&1 || { echo "ABORT: qemu-system-x86_64 not installed" >&2; exit 2; }
        command -v python3 >/dev/null 2>&1 || { echo "ABORT: python3 not installed (JSON parsing)" >&2; exit 2; }

        SER=/tmp/aerosls_p2_net_ser
        rm -f "$SER.in" "$SER.out" "$LOG"
        mkfifo "$SER.in" "$SER.out" 2>/dev/null || true
        cat "$SER.out" > "$LOG" &
        CATPID=$!

        # Host port: the first free loopback one, never a fixed number.
        PORT=$(bash tests/free_port.sh) || { echo "ABORT: no free loopback port for the QEMU hostfwd (see tests/free_port.sh)" >&2; exit 2; }
        BASE="http://127.0.0.1:$PORT"
        W="$(mktemp -d)"
        qemu_err() { [ -s "$W/qemu.err" ] && sed 's/^/      qemu: /' "$W/qemu.err" >&2; }

        # No disk is attached — this arm asserts nothing about storage (the
        # same choice every console/boot guard makes).
        ACCEL="${QEMU_ACCEL:-}"
        if [ -z "$ACCEL" ]; then
            if [ -e /dev/kvm ] && [ -r /dev/kvm ]; then ACCEL="-accel kvm"; else ACCEL="-accel tcg,thread=multi"; fi
        fi

        qemu-system-x86_64 -cdrom "$ISO" \
            -netdev user,id=net0,hostfwd=tcp:127.0.0.1:$PORT-:3000 \
            -device e1000,netdev=net0,mac=52:54:00:12:34:07 \
            -display none -m 4G -smp "$SMP" -boot d -no-reboot \
            $ACCEL \
            -serial pipe:"$SER" 2>"$W/qemu.err" &
        QPID=$!

        cleanup() { kill ${QPID:-} 2>/dev/null; kill ${CATPID:-} 2>/dev/null; rm -rf ${W:-}; }
        trap cleanup EXIT
        trap 'cleanup; exit 1' TERM INT

        bash tests/grub_select_kernel_only.sh "$SER.in" "$LOG" "$QPID" "$ENTRY" || {
            echo "FAILED: could not select grub entry $ENTRY (QEMU or grub failed)" >&2
            qemu_err
            exit 1
        }

        hb_now() { grep -ac "\[INIT\] heartbeat" "$LOG" 2>/dev/null || true; }
        ready=0
        qemu_alive=1
        for i in $(seq 1 $((WINDOW_S * 2))); do
            if [ "$(hb_now)" -ge 5 ] && grep -aq "Listening on port 3000" "$LOG" 2>/dev/null; then
                ready=1
                break
            fi
            if ! kill -0 "$QPID" 2>/dev/null; then qemu_alive=0; break; fi
            sleep 0.5
        done
        if [ "$ready" -ne 1 ]; then
            if [ "$qemu_alive" -eq 0 ]; then
                echo "FAILED: QEMU exited before the boot reached its markers — the boot crashed" >&2
                qemu_err
            else
                echo "FAILED: markers missing within ${WINDOW_S}s (heartbeats: $(hb_now))" >&2
            fi
            tail -20 "$LOG" 2>/dev/null | sed 's/^/      /' >&2
            exit 1
        fi
        echo "ok:   L0. the unified boot is up (control plane serving, init making progress)"
        # ── HTTP helpers (the same shapes the E6 attach guard uses) ──────
        api() {   # api <out-file> <curl args...> — out file CLEARED first, so
            # a request that never landed reads as empty, never as the PREVIOUS
            # request's stale success.
            local out="$1"; shift
            : > "$out"
            local t
            for t in 1 2 3 4 5; do
                if curl -sf --max-time 30 -H "Authorization: Bearer $TOKEN" \
                        -H "Content-Type: application/json" -o "$out" "$@" 2>/dev/null; then
                    return 0
                fi
                sleep 2
            done
            return 1
        }
        jval() {   # jval <json-file> <key> — the value, or empty when absent/not JSON
            python3 - "$1" "$2" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    print(""); sys.exit(0)
v = d.get(sys.argv[2]) if isinstance(d, dict) else None
if v is None:            print("")
elif v is True:          print("true")
elif v is False:         print("false")
else:                    print(v)
PY
        }
        env_field() {   # env_field <envs.json> <env_id> <field>
            python3 - "$1" "$2" "$3" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
for e in d.get("envs", []):
    if str(e.get("env_id")) == sys.argv[2]:
        v = e.get(sys.argv[3])
        sys.stdout.write("" if v is None else str(v))
        break
PY
        }
        console_out() {   # stdout: the `output` field of $W/cons.json
            python3 - "$W/cons.json" <<'PY' 2>/dev/null
import json, sys
try:
    d = json.load(open(sys.argv[1]))
except Exception:
    sys.exit(0)
v = d.get("output") if isinstance(d, dict) else None
if isinstance(v, str):
    sys.stdout.write(v)
PY
        }
        envs_snapshot() { api "$W/envs.json" "$BASE/api/partition/$pid/env" || true; }

        # ── the tenant: one partition, one environment (tenant profile) ────
        api "$W/health.json" "$BASE/api/health" || true
        if [ "$(jval "$W/health.json" status)" != "ok" ]; then
            echo "FAILED: /api/health did not answer a live body (see $LOG)" >&2
            exit 1
        fi
        api "$W/pcreate.json" -X POST -d '{"name":"p2net"}' "$BASE/api/partitions" || true
        pid="$(jval "$W/pcreate.json" partition_id)"
        if [ "$(jval "$W/pcreate.json" ok)" != "true" ] || [ -z "$pid" ] || [ "$pid" = "0" ] || [ "$pid" = "4294967295" ]; then
            echo "FAILED: POST /api/partitions did not define a partition (error='$(jval "$W/pcreate.json" error)')" >&2
            exit 1
        fi
        api "$W/env.json" -X POST -d '{"index":1}' "$BASE/api/partition/$pid/env" || true
        E1="$(jval "$W/env.json" env_id)"
        if [ "$(jval "$W/env.json" ok)" != "true" ] || [ -z "$E1" ] || [ "$E1" = "0" ]; then
            echo "FAILED: could not create environment 1 in partition $pid (env_id='$E1' error='$(jval "$W/env.json" error)')" >&2
            exit 1
        fi
        echo "ok:   L0. environment placed in partition $pid (env_id $E1, index 1)"

        # Bound: the create round-trips through the manager's channel, so a
        # console that is index-less moments after the create is mid-flight.
        IDX=""; SPID=""
        for i in $(seq 1 120); do
            envs_snapshot
            IDX="$(env_field "$W/envs.json" "$E1" index)"
            SPID="$(env_field "$W/envs.json" "$E1" posix_pid)"
            if [ -n "$IDX" ] && [ -n "$SPID" ]; then break; fi
            sleep 0.5
        done
        if [ "$IDX" != "1" ] || [ -z "$SPID" ]; then
            echo "FAILED: the console never bound to index 1 (index='$IDX' posix_pid='$SPID')" >&2
            exit 1
        fi
        SIDE="aerosls.posix.$IDX"
        echo "ok:   L0. console bound: index $IDX, posix pid $SPID ($SIDE)"

        # ── console plumbing: ONE stream, drained through the attach surface ─
        S1=""
        drain_once() {
            api "$W/cons.json" "$BASE/api/partition/$pid/env/$E1/console" || true
            if [ "$(jval "$W/cons.json" ok)" = "true" ]; then
                S1="$S1$(console_out)"
                return 0
            fi
            return 1
        }
        wait_for() {   # wait_for <marker> <seconds> — 0 once the stream shows it
            local marker="$1" secs="$2" i
            for i in $(seq 1 $((secs * 2))); do
                drain_once || true
                case "$S1" in *"$marker"*) return 0 ;; esac
                sleep 0.5
            done
            return 1
        }
        console_send() {   # console_send <line> — waits for the peer to take it
            local line="$1" t ok queued
            python3 - "$line" > "$W/send_body.json" <<'PY'
import json, sys
print(json.dumps({"input": sys.argv[1]}))
PY
            for t in $(seq 1 20); do
                api "$W/send.json" -X POST -d "$(cat "$W/send_body.json")" \
                    "$BASE/api/partition/$pid/env/$E1/console" || true
                ok="$(jval "$W/send.json" ok)"
                queued="$(jval "$W/send.json" queued)"
                if [ "$ok" = "true" ] && [ -n "$queued" ] && [ "$queued" != "0" ]; then return 0; fi
                if [ "$ok" = "false" ]; then
                    echo "FAILED: writing to environment console $E1 was refused ('$(jval "$W/send.json" error)')" >&2
                    return 1
                fi
                sleep 1   # ok=true, queued=0: the peer has not drained yet
            done
            echo "FAILED: environment console $E1 never accepted a line (20s queued=0)" >&2
            return 1
        }
        wait_log() {   # wait_log <fixed string> <seconds> — 0 once LOG carries it
            local pat="$1" secs="$2" i
            for i in $(seq 1 $((secs * 4))); do
                grep -aqF -- "$pat" "$LOG" 2>/dev/null && return 0
                kill -0 "$QPID" 2>/dev/null || return 1
                sleep 0.25
            done
            return 1
        }
        # ── L1: the channel registered under the kernel-owned name ────────
        # Registration happens inside cap_create_sidecar at the create above;
        # kernel TX, so it cannot be lost to a console race. Its pid must be
        # the control plane's posix_pid: the kernel's own account of the
        # wiring and the API's must name the same sidecar.
        WIRE_PAT="[NET-SOCKET] $SIDE (partition $pid, pid $SPID) wired: kernel ends"
        if wait_log "$WIRE_PAT" "$WAIT_S"; then
            echo "ok:   L1. the network cap registered under the kernel-owned name ($SIDE, partition $pid, pid $SPID)"
        else
            echo "FAILED: L1. no '[NET-SOCKET] $SIDE (partition $pid, pid $SPID) wired:' in $LOG — the tenant's network cap never reached the service (last serial lines):" >&2
            grep -a "NET-SOCKET\|SIDECAR" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi

        # ── L2: the console's nc reaches the service and is refused BY NAME ─
        # socket() itself is refused — no peer, no connect, nothing admitted —
        # so TEST-NET-1 (192.0.2.1) is the whole point: the address must never
        # matter. The marker after it proves the shell ran both commands.
        console_send "nc 192.0.2.1 80" || exit 1
        console_send "echo P2-NC-DONE" || exit 1
        if ! wait_for "P2-NC-DONE" "$ATTACH_WAIT_S"; then
            echo "FAILED: the tenant console never ran the commands (no P2-NC-DONE after ${ATTACH_WAIT_S}s)" >&2
            exit 1
        fi
        echo "ok:   L0. the tenant console ran nc (marker P2-NC-DONE seen)"

        REF_PAT="[NET-SOCKET] $SIDE (partition $pid, pid $SPID): "
        if ! wait_log "$REF_PAT" "$WAIT_S"; then
            echo "FAILED: L2. no refusal line for $SIDE (partition $pid, pid $SPID) in $LOG after ${WAIT_S}s — nc's NET_SOCKET never reached the service" >&2
            grep -a "NET-SOCKET" "$LOG" 2>/dev/null | tail -10 | sed 's/^/      /' >&2
            exit 1
        fi
        REFLINE="$(grep -aF "$REF_PAT" "$LOG" | grep -aF 'NET_SOCKET refused by kernel.net.socket' | head -1)"
        if [ -z "$REFLINE" ]; then
            echo "FAILED: L2. the service answered $SIDE, but not with a refusal naming itself (lines that reached it):" >&2
            grep -aF "$REF_PAT" "$LOG" | tail -5 | sed 's/^/      /' >&2
            exit 1
        fi
        case "$REFLINE" in
            *"NET_SOCKET refused by kernel.net.socket — nothing admits yet (P2 increment 1)"*)
                echo "ok:   L2. nc reached the service and was refused BY NAME ($REFLINE)"
                ;;
            *)
                echo "FAILED: L2. the refusal line is not the pinned rendering: $REFLINE" >&2
                exit 1
                ;;
        esac

        # ── L3: every handshake got the honest NET_INFO answer ─────────────
        # A service that never replies makes each sidecar's bounded retry loop
        # print this line before giving up; the honest answer must keep it
        # absent for every environment in the boot, E3's included.
        if grep -aqF "[POSIX] NETBOOT FAILED" "$LOG"; then
            echo "FAILED: L3. an environment's NET_INFO handshake went unanswered — the service must answer it honestly:" >&2
            grep -aF "[POSIX] NETBOOT FAILED" "$LOG" | tail -5 | sed 's/^/      /' >&2
            fail=1
        else
            echo "ok:   L3. no environment reports NETBOOT FAILED — every handshake was answered"
        fi
        :
    fi
fi

exit "$fail"
